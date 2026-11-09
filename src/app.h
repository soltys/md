#ifndef MD_APP_H
#define MD_APP_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "editor.h"
#include "nvim.h"
#include "view.h"

#define APP_NAME "md"

typedef enum Layout {
    LAYOUT_SPLIT,        /* editor + viewer */
    LAYOUT_VIEWER,       /* viewer only (split disabled) */
    LAYOUT_EDITOR,       /* editor only */
} Layout;

typedef enum Pane { PANE_EDITOR, PANE_VIEWER } Pane;

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_TextEngine *engine;
    View view;
    Editor editor;
    Nvim nvim;
    bool nvim_started;   /* Neovim is launched on first use of the editor */
    bool nvim_ok;        /* started and running */
    const char *start_file; /* file from the command line */

    SDL_Thread *font_thread;
    bool fonts_ok;

    /* Most recent document finished by a parser thread, not yet shown.
     * Exchanged atomically between the worker and the main thread. */
    void *loaded_doc;
    SDL_AtomicInt loads_pending;
    Uint32 wake_event;

    /* Live preview: at most one parse of editor text in flight. */
    bool preview_parsing;
    char *preview_pending_text;
    size_t preview_pending_len;
    char *preview_pending_name;

    Layout layout;
    bool vertical;       /* true: side by side, false: stacked */
    bool editor_second;  /* editor on the right / bottom instead of left / top */
    Pane focus;
    SDL_FRect editor_rect, viewer_rect;
    bool mouse_down_editor;

    /* Viewer text selection by mouse. */
    bool selecting;      /* left button went down in the viewer */
    bool sel_dragged;    /* moved far enough to count as a drag */
    float press_x, press_y;

    /* Short status message ("Copied ...") drawn over the viewer. */
    char toast[96];
    Uint64 toast_until;  /* SDL_GetTicks() deadline */

    bool dark;
    bool dirty;
    bool quitting;
} App;

#endif
