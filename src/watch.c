/*
 * Auto-reload: one thread waits for changes to the open file.
 *
 * On Windows it sleeps on a directory change notification for the file's
 * folder (FindFirstChangeNotificationW), so nothing runs while the file is
 * left alone. Editors often save by writing a temporary file and renaming
 * it, or in several writes; changes are therefore collected until the
 * folder has been quiet for a moment, and only then the file's size and
 * modification time are compared with what was last seen. Elsewhere (and
 * if the notification cannot be set up, e.g. on some network shares) the
 * thread checks the file twice a second instead.
 *
 * The thread owns its state and frees it when told to stop, so stopping
 * never waits for it.
 */
#include "watch.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define QUIET_MS 120      /* folder quiet this long: look at the file */
#define MAX_SETTLE_MS 600 /* but no later than this after the first change */
#define POLL_MS 500

typedef struct Watch {
    char *path;
    Uint32 wake_event;
    SDL_AtomicInt stop;
#ifdef _WIN32
    HANDLE stop_event;
#else
    SDL_Semaphore *stop_sem;
#endif
    bool exists;
    SDL_Time mtime;
    Uint64 size;
} Watch;

static Watch *current;

static void watch_free(Watch *w)
{
#ifdef _WIN32
    if (w->stop_event) CloseHandle(w->stop_event);
#else
    SDL_DestroySemaphore(w->stop_sem);
#endif
    SDL_free(w->path);
    SDL_free(w);
}

static void remember(Watch *w)
{
    SDL_PathInfo info;
    w->exists = SDL_GetPathInfo(w->path, &info) && info.type == SDL_PATHTYPE_FILE;
    w->mtime = w->exists ? info.modify_time : 0;
    w->size = w->exists ? info.size : 0;
}

/* Tells the main thread when the file is there and differs from last time.
 * A file that is missing for a moment (in the middle of a rename) is not a
 * change; it counts once it is back. */
static void check(Watch *w)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(w->path, &info) || info.type != SDL_PATHTYPE_FILE) {
        w->exists = false;
        return;
    }
    if (w->exists && info.modify_time == w->mtime && info.size == w->size) return;
    w->exists = true;
    w->mtime = info.modify_time;
    w->size = info.size;
    if (SDL_GetAtomicInt(&w->stop)) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = w->wake_event;
    ev.user.code = WATCH_EVENT_CODE;
    ev.user.data1 = SDL_strdup(w->path);
    if (!ev.user.data1 || !SDL_PushEvent(&ev)) SDL_free(ev.user.data1);
}

/* Sleeps up to ms; returns false when the watch was stopped. */
static bool wait_stop(Watch *w, Sint32 ms)
{
#ifdef _WIN32
    return WaitForSingleObject(w->stop_event, (DWORD)ms) == WAIT_TIMEOUT && !SDL_GetAtomicInt(&w->stop);
#else
    return !SDL_WaitSemaphoreTimeout(w->stop_sem, ms) && !SDL_GetAtomicInt(&w->stop);
#endif
}

#ifdef _WIN32
static HANDLE open_folder_notification(const char *path)
{
    /* The folder: everything before the last separator, else ".". */
    const char *slash = NULL;
    for (const char *q = path; *q; ++q) if (*q == '/' || *q == '\\') slash = q;
    char *dir = slash ? SDL_strndup(path, (size_t)(slash - path) + (slash == path || slash[-1] == ':'))
                      : SDL_strdup(".");
    if (!dir) return INVALID_HANDLE_VALUE;
    HANDLE h = INVALID_HANDLE_VALUE;
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    WCHAR *wdir = n > 0 ? SDL_malloc(sizeof(WCHAR) * (size_t)n) : NULL;
    if (wdir && MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, n) > 0) {
        h = FindFirstChangeNotificationW(wdir, FALSE,
                                         FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE);
    }
    SDL_free(wdir);
    SDL_free(dir);
    return h;
}
#endif

static int SDLCALL watch_thread_main(void *arg)
{
    Watch *w = arg;
#ifdef _WIN32
    HANDLE change = open_folder_notification(w->path);
    if (change != INVALID_HANDLE_VALUE) {
        HANDLE hs[2] = {w->stop_event, change};
        while (!SDL_GetAtomicInt(&w->stop)) {
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
            /* Something in the folder changed: wait for it to settle. */
            const Uint64 first = SDL_GetTicks();
            DWORD r;
            do {
                if (!FindNextChangeNotification(change)) goto done;
                r = WaitForMultipleObjects(2, hs, FALSE, QUIET_MS);
            } while (r == WAIT_OBJECT_0 + 1 && SDL_GetTicks() - first < MAX_SETTLE_MS);
            if (r == WAIT_OBJECT_0 || SDL_GetAtomicInt(&w->stop)) break;
            /* After the time limit the notification may still be signalled:
             * then the loop comes straight back for the rest. */
            check(w);
        }
    done:
        FindCloseChangeNotification(change);
        watch_free(w);
        return 0;
    }
#endif
    while (wait_stop(w, POLL_MS)) check(w);
    watch_free(w);
    return 0;
}

void watch_stop(void)
{
    Watch *w = current;
    current = NULL;
    if (!w) return;
    SDL_SetAtomicInt(&w->stop, 1);
#ifdef _WIN32
    SetEvent(w->stop_event);
#else
    SDL_SignalSemaphore(w->stop_sem);
#endif
}

void watch_file(const char *path, Uint32 wake_event)
{
    if (current && path && SDL_strcmp(current->path, path) == 0) return;
    watch_stop();
    if (!path || !path[0] || !wake_event) return;
    Watch *w = SDL_calloc(1, sizeof(Watch));
    if (!w) return;
    w->path = SDL_strdup(path);
    w->wake_event = wake_event;
#ifdef _WIN32
    w->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!w->stop_event) { watch_free(w); return; }
#else
    w->stop_sem = SDL_CreateSemaphore(0);
    if (!w->stop_sem) { watch_free(w); return; }
#endif
    if (!w->path) { watch_free(w); return; }
    remember(w); /* the state the open document was read in */
    SDL_Thread *t = SDL_CreateThread(watch_thread_main, "md-watch", w);
    if (!t) { watch_free(w); return; }
    SDL_DetachThread(t);
    current = w;
}
