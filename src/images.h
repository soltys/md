#ifndef MD_IMAGES_H
#define MD_IMAGES_H

#include <stdbool.h>
#include <SDL3/SDL.h>

/* Images referenced by the document, decoded on worker threads. */
typedef enum { IMAGE_LOADING, IMAGE_READY, IMAGE_FAILED } ImageState;

typedef struct Image {
    char *key;              /* resolved path */
    ImageState state;
    SDL_Texture *texture;
    int w, h;
    SDL_Surface *surface;   /* handed over by the worker */
} Image;

void images_init(Uint32 wake_event);
/* Returns the cache entry for `url` relative to the document at doc_path,
 * starting a background load the first time. Remote URLs fail. */
Image *images_get(const char *doc_path, const char *url, size_t url_len);
/* Uploads finished decodes to textures; true if any image changed. */
bool images_pump(SDL_Renderer *r);
/* True while any image is still being decoded (or not yet uploaded). */
bool images_loading(void);
void images_shutdown(void);

#endif
