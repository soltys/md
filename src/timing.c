#include "timing.h"

#if MD_TIMING

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

#define MAX_ENTRIES 2048

typedef struct Entry {
    Uint64 at;          /* counter value when the step ended */
    Uint64 dur;         /* counter ticks since the thread's previous mark */
    const char *thread;
    char what[96];
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count, dropped;
static SDL_Mutex *lock;
static Uint64 base, freq;      /* counter value at process creation; ticks per second */
static SDL_AtomicInt reported;
static FILE *log_file;
static char log_path[1024];
#ifdef _WIN32
static HANDLE out = INVALID_HANDLE_VALUE;
static bool out_is_console;
#endif

static THREAD_LOCAL const char *thread_name;
static THREAD_LOCAL Uint64 thread_last;

static Uint64 now(void)
{
#ifdef _WIN32
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (Uint64)c.QuadPart;
#else
    return SDL_GetPerformanceCounter();
#endif
}

static double ms(Uint64 ticks) { return (double)ticks * 1000.0 / (double)freq; }

/* ------------------------------------------------------------ output -- */

static void open_outputs(void)
{
    char *path = log_path;
    const size_t path_size = sizeof log_path;
    SDL_strlcpy(path, "md-timing.log", path_size);
#ifdef _WIN32
    char tmp[MAX_PATH + 1];
    DWORD n = GetTempPathA(sizeof tmp, tmp);
    if (n > 0 && n < sizeof tmp) SDL_snprintf(path, path_size, "%smd-timing.log", tmp);

    /* stderr redirected to a file or pipe (md 2> log.txt), otherwise the
     * console md was started from, if any. md is a GUI program, so the
     * shell does not wait for it: the lines land after the prompt. */
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD type = h && h != INVALID_HANDLE_VALUE ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        out = h;
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
        out_is_console = out != INVALID_HANDLE_VALUE;
    }
#else
    const char *tmp = SDL_getenv("TMPDIR");
    SDL_snprintf(path, path_size, "%s/md-timing.log", tmp && *tmp ? tmp : "/tmp");
#endif
    log_file = fopen(path, "w");
}

static void write_out(const char *text)
{
#ifdef _WIN32
    OutputDebugStringA(text); /* Visual Studio's Output window, DebugView */
    if (out != INVALID_HANDLE_VALUE) {
        DWORD written;
        if (out_is_console) WriteConsoleA(out, text, (DWORD)SDL_strlen(text), &written, NULL);
        else WriteFile(out, text, (DWORD)SDL_strlen(text), &written, NULL);
    }
#else
    fputs(text, stderr);
    fflush(stderr);
#endif
    if (log_file) {
        fputs(text, log_file);
        fflush(log_file);
    }
}

static void format_entry(char *buf, size_t size, const Entry *e)
{
    SDL_snprintf(buf, size, "%10.3f %10.3f  %-8s %s\n",
                 ms(e->at - base), ms(e->dur), e->thread, e->what);
}

/* ------------------------------------------------------------ record -- */

static void record(bool event, const char *fmt, va_list ap)
{
    Uint64 t = now();
    Entry e;
    e.at = t;
    e.dur = thread_last ? t - thread_last : 0;
    e.thread = thread_name ? thread_name : "?";
    SDL_vsnprintf(e.what, sizeof e.what, fmt, ap);
    thread_last = t;

    SDL_LockMutex(lock);
    if (SDL_GetAtomicInt(&reported)) {
        if (event) {
            char line[192];
            format_entry(line, sizeof line, &e);
            write_out(line);
        }
    } else if (entry_count < MAX_ENTRIES) {
        entries[entry_count++] = e;
    } else {
        dropped++;
    }
    SDL_UnlockMutex(lock);
}

void timing_init(void)
{
    Uint64 t = now();
#ifdef _WIN32
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq = (Uint64)f.QuadPart;
    /* Time spent before main(): image loading, static CRT and SDL_main. */
    FILETIME created, exited, kernel, user, wall;
    Uint64 before_main = 0;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&wall);
        Uint64 c = ((Uint64)created.dwHighDateTime << 32) | created.dwLowDateTime;
        Uint64 w = ((Uint64)wall.dwHighDateTime << 32) | wall.dwLowDateTime;
        /* 100 ns units; split the division so the product cannot overflow */
        if (w > c) before_main = (w - c) / 10000000u * freq + (w - c) % 10000000u * freq / 10000000u;
    }
    base = t - SDL_min(before_main, t);
#else
    freq = SDL_GetPerformanceFrequency();
    base = t;
#endif
    lock = SDL_CreateMutex();
    thread_name = "main";
    thread_last = base;
    timing_mark("process start -> main()");
}

void timing_thread(const char *name)
{
    thread_name = name;
    thread_last = 0; /* the start mark has no duration of its own */
    timing_mark("thread started");
}

void timing_mark(const char *fmt, ...)
{
    if (SDL_GetAtomicInt(&reported)) return; /* startup steps only */
    va_list ap;
    va_start(ap, fmt);
    record(false, fmt, ap);
    va_end(ap);
}

void timing_event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    record(true, fmt, ap);
    va_end(ap);
}

static int SDLCALL by_time(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at;
}

const char *timing_log_path(void) { return log_path; }

double timing_first_full_render(void)
{
    if (SDL_GetAtomicInt(&reported)) return -1.0;
    timing_mark("== first full render ==");
    const double total = ms(thread_last - base);
    SDL_LockMutex(lock);
    SDL_SetAtomicInt(&reported, 1);
    Entry *list = SDL_malloc(sizeof(Entry) * (size_t)(entry_count ? entry_count : 1));
    int count = entry_count;
    if (list) SDL_memcpy(list, entries, sizeof(Entry) * (size_t)count);
    else count = 0;
    SDL_UnlockMutex(lock);

    SDL_qsort(list, (size_t)count, sizeof(Entry), by_time);
    open_outputs();

    size_t cap = (size_t)count * 160 + 1024, len = 0;
    char *text = SDL_malloc(cap);
    if (!text) { SDL_free(list); return total; }
    len += (size_t)SDL_snprintf(text + len, cap - len,
        "\nmd startup timing (ms since process creation; step = time since the "
        "previous mark on the same thread)\n"
        "%10s %10s  %-8s %s\n", "total", "step", "thread", "what");
    for (int i = 0; i < count; ++i) {
        format_entry(text + len, cap - len, &list[i]);
        len += SDL_strlen(text + len);
    }
    if (dropped) {
        len += (size_t)SDL_snprintf(text + len, cap - len, "(%d more marks dropped)\n", dropped);
    }
    SDL_snprintf(text + len, cap - len, "(log file: %s)\n-- later events --\n",
                 log_file ? log_path : "could not be created");
    write_out(text);
    SDL_free(text);
    SDL_free(list);
    return total;
}

#else

typedef int md_timing_disabled; /* keeps the translation unit non-empty (C4206) */

#endif
