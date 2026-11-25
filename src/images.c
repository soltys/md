#include "images.h"
#include <SDL3_image/SDL_image.h>
#include "timing.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#ifndef MD_VERSION
#define MD_VERSION "0.0.0-dev"
#endif
#define MD_WIDEN2(s) L##s
#define MD_WIDEN(s) MD_WIDEN2(s)
#define MD_VERSION_W MD_WIDEN(MD_VERSION)

/* Remote images larger than this are not downloaded. */
#define REMOTE_MAX_BYTES (32u * 1024u * 1024u)

static Image **cache;
static int cache_count, cache_cap;
static SDL_Mutex *lock;
static Uint32 wake;
static SDL_AtomicInt done_count;

typedef struct Job { Image *img; char *path; bool remote; } Job;

static bool is_remote(const char *url, size_t n)
{
    return (n > 7 && SDL_strncasecmp(url, "http://", 7) == 0) ||
           (n > 8 && SDL_strncasecmp(url, "https://", 8) == 0);
}

#ifdef _WIN32
/* Downloads `url` with WinHTTP (delay-loaded, so it costs nothing until the
 * first remote image). Runs on the image worker thread. Follows redirects,
 * uses the system proxy settings, and gives up on anything but HTTP 200. */
static void *http_get(const char *url, size_t *out_len)
{
    void *result = NULL;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    char *buf = NULL;
    size_t len = 0, cap = 0;

    int wn = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    if (wn <= 0) return NULL;
    wchar_t *wurl = SDL_malloc(sizeof(wchar_t) * (size_t)wn);
    if (!wurl) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wn);

    URL_COMPONENTS uc;
    SDL_zero(uc);
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.lpszHostName || !uc.dwHostNameLength) goto out;
    wchar_t host[256];
    if (uc.dwHostNameLength >= SDL_arraysize(host)) goto out;
    SDL_memcpy(host, uc.lpszHostName, sizeof(wchar_t) * uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;
    /* Path and query are contiguous in the URL; the fragment was stripped. */
    const wchar_t *path = uc.dwUrlPathLength ? uc.lpszUrlPath
                        : uc.dwExtraInfoLength ? uc.lpszExtraInfo : L"/";

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
    const wchar_t *agent = L"md/" MD_VERSION_W;
    session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) /* before Windows 8.1 */
        session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto out;
    /* resolve, connect, send, receive (ms) */
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
#endif

    conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) goto out;
    req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto out;
    if (!WinHttpSendRequest(req, L"Accept: image/*, */*;q=0.5\r\n", (DWORD)-1L,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto out;
    if (!WinHttpReceiveResponse(req, NULL)) goto out;

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        goto out;

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto out;
        if (avail == 0) break;
        if (len + avail > REMOTE_MAX_BYTES) goto out;
        if (len + avail > cap) {
            size_t ncap = cap ? cap : 64 * 1024;
            while (ncap < len + avail) ncap *= 2;
            char *nb = SDL_realloc(buf, ncap);
            if (!nb) goto out;
            buf = nb;
            cap = ncap;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf + len, avail, &got)) goto out;
        if (got == 0) break;
        len += got;
    }
    if (len) {
        result = buf;
        buf = NULL;
        *out_len = len;
    }
out:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    SDL_free(buf);
    SDL_free(wurl);
    return result;
}
#else
/* Non-Windows builds are for development only: no downloader. */
static void *http_get(const char *url, size_t *out_len)
{
    (void)url;
    (void)out_len;
    return NULL;
}
#endif

static int SDLCALL load_main(void *arg)
{
    Job *job = arg;
    TIMING_THREAD("image");
    SDL_Surface *s = NULL;
    if (job->remote) {
        size_t len = 0;
        void *data = http_get(job->path, &len);
        TIMING_EVENT("download image %s (%u bytes)", job->path, (unsigned)len);
        if (data) {
            SDL_IOStream *io = SDL_IOFromConstMem(data, len);
            if (io) s = IMG_Load_IO(io, true);
            SDL_free(data);
        }
    } else {
        s = IMG_Load(job->path);
    }
    if (s && s->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        s = c;
    }
    TIMING_EVENT("decode image %s", job->path);
    SDL_LockMutex(lock);
    job->img->surface = s;
    if (!s) job->img->state = IMAGE_FAILED;
    SDL_UnlockMutex(lock);
    SDL_AddAtomicInt(&done_count, 1);
    SDL_free(job->path);
    SDL_free(job);
    if (wake) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = wake;
        SDL_PushEvent(&ev);
    }
    return 0;
}

void images_init(Uint32 wake_event)
{
    wake = wake_event;
    lock = SDL_CreateMutex();
}

static char *resolve(const char *doc_path, const char *url, size_t n)
{
    char *u = SDL_strndup(url, n);
    if (is_remote(url, n)) {
        /* Keep the URL as written (query included); drop the #fragment. */
        for (char *q = u; *q; ++q) if (*q == '#') { *q = 0; break; }
        return u;
    }
    /* Strip a "file://" prefix and any #fragment / ?query. */
    char *p = u;
    if (SDL_strncasecmp(p, "file:///", 8) == 0) p += 8;
    else if (SDL_strncasecmp(p, "file://", 7) == 0) p += 7;
    for (char *q = p; *q; ++q) if (*q == '#' || *q == '?') { *q = 0; break; }
    /* Percent-decode. */
    char *w = p;
    for (char *q = p; *q; ++q) {
        if (q[0] == '%' && SDL_isxdigit(q[1]) && SDL_isxdigit(q[2])) {
            char hex[3] = {q[1], q[2], 0};
            *w++ = (char)SDL_strtol(hex, NULL, 16);
            q += 2;
        } else {
            *w++ = *q;
        }
    }
    *w = 0;

    bool absolute = p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
    char *out = NULL;
    if (absolute || !doc_path || !*doc_path) {
        out = SDL_strdup(p);
    } else {
        const char *slash = NULL;
        for (const char *q = doc_path; *q; ++q) if (*q == '/' || *q == '\\') slash = q;
        if (slash) SDL_asprintf(&out, "%.*s/%s", (int)(slash - doc_path), doc_path, p);
        else out = SDL_strdup(p);
    }
    SDL_free(u);
    return out;
}

Image *images_get(const char *doc_path, const char *url, size_t url_len)
{
    char *key = resolve(doc_path, url, url_len);
    for (int i = 0; i < cache_count; ++i) {
        if (SDL_strcmp(cache[i]->key, key) == 0) {
            SDL_free(key);
            return cache[i];
        }
    }
    Image *img = SDL_calloc(1, sizeof(Image));
    img->key = key;
    if (cache_count == cache_cap) {
        cache_cap = cache_cap ? cache_cap * 2 : 16;
        cache = SDL_realloc(cache, sizeof(Image *) * (size_t)cache_cap);
    }
    cache[cache_count++] = img;

    /* http(s) images are downloaded on the worker thread; other schemes
     * (ftp:, data:, ...) are not supported and show their alt text. */
    bool remote = is_remote(url, url_len);
    bool other_scheme = !remote && SDL_strstr(key, "://") && SDL_strncasecmp(url, "file:", 5) != 0;
    if (other_scheme || url_len == 0) {
        img->state = IMAGE_FAILED;
        return img;
    }
    img->state = IMAGE_LOADING;
    Job *job = SDL_malloc(sizeof(Job));
    job->img = img;
    job->path = SDL_strdup(key);
    job->remote = remote;
    SDL_Thread *t = SDL_CreateThread(load_main, "md-image", job);
    if (t) SDL_DetachThread(t);
    else load_main(job);
    return img;
}

bool images_pump(SDL_Renderer *r)
{
    if (SDL_GetAtomicInt(&done_count) == 0) return false;
    SDL_SetAtomicInt(&done_count, 0);
    bool changed = false;
    SDL_LockMutex(lock);
    for (int i = 0; i < cache_count; ++i) {
        Image *img = cache[i];
        if (img->state == IMAGE_LOADING && img->surface) {
            img->texture = SDL_CreateTextureFromSurface(r, img->surface);
            img->w = img->surface->w;
            img->h = img->surface->h;
            SDL_DestroySurface(img->surface);
            img->surface = NULL;
            img->state = img->texture ? IMAGE_READY : IMAGE_FAILED;
            if (img->texture) SDL_SetTextureScaleMode(img->texture, SDL_SCALEMODE_LINEAR);
            changed = true;
        } else if (img->state == IMAGE_FAILED && img->w == 0) {
            img->w = -1; /* report each failure once */
            changed = true;
        }
    }
    SDL_UnlockMutex(lock);
    return changed;
}

bool images_loading(void)
{
    bool loading = false;
    SDL_LockMutex(lock);
    for (int i = 0; i < cache_count && !loading; ++i) loading = cache[i]->state == IMAGE_LOADING;
    SDL_UnlockMutex(lock);
    return loading;
}

void images_shutdown(void)
{
    /* Worker threads may still run; leave entries they reference alone. */
    for (int i = 0; i < cache_count; ++i) {
        if (cache[i]->texture) SDL_DestroyTexture(cache[i]->texture);
        cache[i]->texture = NULL;
    }
}
