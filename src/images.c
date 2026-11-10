#include "images.h"
#include <SDL3_image/SDL_image.h>

static Image **cache;
static int cache_count, cache_cap;
static SDL_Mutex *lock;
static Uint32 wake;
static SDL_AtomicInt done_count;

typedef struct Job { Image *img; char *path; } Job;

static int SDLCALL load_main(void *arg)
{
    Job *job = arg;
    SDL_Surface *s = IMG_Load(job->path);
    if (s && s->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        s = c;
    }
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

    bool remote = SDL_strstr(url, "://") && SDL_strncasecmp(url, "file:", 5) != 0;
    if (remote || url_len == 0) {
        img->state = IMAGE_FAILED; /* no network access by design */
        return img;
    }
    img->state = IMAGE_LOADING;
    Job *job = SDL_malloc(sizeof(Job));
    job->img = img;
    job->path = SDL_strdup(key);
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
