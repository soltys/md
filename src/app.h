#ifndef MD_APP_H
#define MD_APP_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "view.h"

#define APP_NAME "md"

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_TextEngine *engine;
    View view;

    SDL_Thread *font_thread;
    bool fonts_ok;

    /* Most recent document finished by a loader thread, not yet shown.
     * Exchanged atomically between the loader and the main thread. */
    void *loaded_doc;
    SDL_AtomicInt loads_pending;
    Uint32 wake_event;

    bool dark;
    bool dirty;
} App;

#endif
