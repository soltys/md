#ifndef MD_APP_H
#define MD_APP_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "editor.h"
#include "nvim.h"
#include "outline.h"
#include "view.h"

#define APP_NAME "md"

typedef enum Layout {
    LAYOUT_SPLIT,        /* editor + viewer */
    LAYOUT_VIEWER,       /* viewer only (split disabled) */
    LAYOUT_EDITOR,       /* editor only */
} Layout;

typedef enum Pane { PANE_EDITOR, PANE_VIEWER } Pane;

/* A place in the back / forward history: a file and how far down it was
 * scrolled. */
typedef struct HistoryEntry {
    char *path;
    float scroll;        /* pixels at zoom 1 */
} HistoryEntry;

#define HISTORY_MAX 64

typedef struct App {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_TextEngine *engine;
    View view;
    Editor editor;
    Nvim nvim;
    bool nvim_started;   /* Neovim is launched on first use of the editor */
    bool nvim_starting;  /* launched, not yet waited for */
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
    SDL_FRect outline_rect;  /* outline panel, left of the preview (Ctrl+Shift+T) */
    Outline outline;
    float zoom;              /* preview zoom, 1.0 = 100% (Ctrl+wheel, Ctrl+= / - / 0) */
    float zoom_wheel;        /* fractional Ctrl+wheel steps (touchpad pinch) */

    /* Back / forward (Alt+Left / Alt+Right, mouse side buttons). */
    HistoryEntry back[HISTORY_MAX], forward[HISTORY_MAX];
    int back_count, forward_count;
    /* Applied to the next document that is not the one on screen: an
     * anchor from "other.md#heading", or the scroll position to return to. */
    char *pending_anchor;
    float pending_scroll;    /* < 0: none */
    bool mouse_down_editor;

    /* Scroll sync. The editor reports its top line (md_scroll); scrolling
     * the viewer sends one back. The echo of a line md sent is ignored. */
    bool viewer_scrolled;
    int sent_top;
    Uint64 sent_at;

    /* Viewer text selection by mouse. */
    bool selecting;      /* left button went down in the viewer */
    bool sel_dragged;    /* moved far enough to count as a drag */
    float press_x, press_y;

    /* Short status message ("Copied ...") drawn over the viewer. */
    char toast[160];
    Uint64 toast_until;  /* SDL_GetTicks() deadline */

    bool show_help;      /* F1 overlay */
    char title[1024];    /* full path of the document, shown in the title bar */

    /* Preview search: "/" or "?" then text, Enter / Esc; n / N repeat. */
    bool searching;
    bool search_back, search_found;
    char search[256];
    char last_search[256];
    bool last_search_back;
    SelPos search_origin;
    float search_scroll0;
    bool dark;
    bool theme_forced;        /* --light / --dark: ignore the system theme */
    bool dirty;
    bool quitting;
} App;

#endif
