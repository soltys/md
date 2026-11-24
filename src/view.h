#ifndef MD_VIEW_H
#define MD_VIEW_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "images.h"
#include "theme.h"

/* A run of text (or an image) placed by the layout engine. Positions are
 * relative to the owning paragraph's origin. TTF_Text objects are created
 * lazily the first time a fragment is drawn. */
typedef struct Frag {
    float x, y, w, h;         /* w includes pad_l / pad_r */
    float pad_l, pad_r;       /* room kept for an inline code background */
    float ax, ay;             /* absolute document position (selection) */
    int item;                 /* owning item index */
    uint32_t str, len;        /* absolute offset into Document.strings */
    TTF_Font *font;
    TTF_Text *text;
    SDL_Color color;
    uint16_t style;
    bool joined;              /* continues the word of the previous fragment (broken
                                 because it was wider than the line) */
    uint32_t url, url_len;
    Image *img;
} Frag;

typedef struct Para {
    int first, count;         /* range in View.frags */
    float w, h;
} Para;

typedef struct ViewCell {
    Para para;
    float x, y;               /* relative to the item origin */
} ViewCell;

typedef struct ViewItem {
    const Block *block;
    Para para;
    Para marker;
    Para lang;
    ViewCell *cells;          /* tables */
    float *col_w;
    float *row_h;
    float x, y, w, h;         /* text box, document coordinates (pixels) */
    float box_y, box_h;       /* background box (code, tables) */
    float hscroll;            /* code blocks: horizontal scroll of long lines */
} ViewItem;

/* A position in the laid-out text: fragment index + byte offset in it.
 * Fragments are stored in document order, so positions compare simply. */
typedef struct SelPos {
    int frag;
    uint32_t off;
} SelPos;

typedef struct View {
    TTF_TextEngine *engine;
    Theme theme;
    Document *doc;
    ViewItem *items;
    int item_count;
    int laid;                 /* items [0, laid) have a layout */
    float next_y;             /* where item `laid` goes */

    Frag *frags;
    int frag_count, frag_cap;

    float scale;              /* display scale, 1.0 = 96 DPI */
    float width, height;      /* viewport, pixels */
    float scroll;
    float content_height;     /* of the laid-out part (grows) */

    TTF_Text *msg_title;
    TTF_Text *msg_body;

    bool has_sel;
    SelPos sel_anchor, sel_focus;

    /* Copy button of code blocks: shown on the block under the mouse. */
    int hover_code;           /* item index, -1 = none */
    bool hover_button;        /* the mouse is on the button itself */
    int copied_item;          /* shows a check mark until cleared, -1 = none */
    TTF_Text *icon_copy, *icon_check;

    /* Body text in Roboto Serif instead of Roboto (F7, --serif, the "Aa"
     * button). Change it with view_set_serif(). */
    bool serif;
    /* The "Aa" font button in the top-right corner, shown while the mouse
     * is over the preview. */
    bool mouse_inside;
    bool hover_font_button;
    TTF_Text *icon_font;
    bool icon_font_serif;     /* which face icon_font was made with */
    /* Overlay scroll bar on the right edge: thin, wider while the mouse is
     * near it or its thumb is being dragged. */
    bool hover_scrollbar;
    bool scrollbar_drag;
    float scrollbar_grab;     /* mouse y minus thumb top, while dragging */
    bool exporting;           /* drawing for F12: no scroll bar */
} View;

void view_init(View *v, TTF_TextEngine *engine, const Theme *theme);
void view_destroy(View *v);

/* Takes ownership of doc (may be NULL). keep_scroll keeps the position
 * (live preview); otherwise the view starts at the top. */
void view_set_document(View *v, Document *doc, bool keep_scroll);

/* New viewport size / scale. Lays out what is visible now; the rest of the
 * document follows in view_layout_more() calls. */
void view_layout(View *v, float width, float height, float scale);

/* Images finished loading: re-layout keeping the position. */
void view_relayout(View *v);

/* Continues the incremental layout for about budget_ms. Returns true while
 * more remains. */
bool view_layout_more(View *v, Uint64 budget_ms);
bool view_layout_pending(const View *v);

void view_scroll_by(View *v, float dy);
/* Scrolls the code block under viewport position (x, y) sideways by dx
 * pixels. Returns false if there is no code block there or its lines all
 * fit (the caller can then use the event for something else). */
bool view_hscroll_at(View *v, float x, float y, float dx);
void view_scroll_to(View *v, float y);
void view_scroll_to_end(View *v);
/* Scrolls so the block containing source line `line` (1-based) is on top. */
void view_scroll_to_line(View *v, int line);
/* Scrolled all the way down (and not at the top). */
bool view_at_end(const View *v);
/* The source line shown at the top of the viewport (1-based). */
int view_top_line(const View *v);
float view_line_height(const View *v);

/* Copy button of code blocks. view_hover updates which block shows the
 * button for the mouse at viewport position (x, y) (pass a negative x when
 * the mouse left the viewer) and returns true when a redraw is needed. */
bool view_hover(View *v, float x, float y);
/* Item index of the copy button under (x, y), -1 if none. */
int view_copy_button_at(View *v, float x, float y);
/* The raw code of a code block item (SDL_free it), NULL if not one. */
char *view_code_text(View *v, int item);
/* Shows a check mark on the item's button (-1 removes it). */
void view_set_copied(View *v, int item);

/* Switches the body text between Roboto and Roboto Serif, keeping the
 * scroll position. Returns true when it changed. */
bool view_set_serif(View *v, bool serif);
/* True when viewport position (x, y) is on the "Aa" font button. */
bool view_font_button_at(View *v, float x, float y);

/* Link under a viewport position (pixels); returns false if none. */
bool view_link_at(View *v, float x, float y, const char **url, size_t *len);

void view_draw(View *v, SDL_Renderer *r);
/* Scroll bar, in viewport pixels. view_scrollbar_press() returns false when
 * (x, y) is not on the bar; on the thumb it starts a drag, on the track it
 * pages towards the mouse. */
bool view_scrollbar_at(View *v, float x, float y);
bool view_scrollbar_press(View *v, float x, float y);
void view_scrollbar_drag(View *v, float y);
void view_scrollbar_release(View *v);
/* Lays out the whole document now (which also requests every image). */
void view_layout_all(View *v);
/* Draws the whole document into a new surface (F12, save as PNG). On
 * failure returns NULL and sets *error. */
SDL_Surface *view_render_surface(View *v, SDL_Renderer *r, const char **error);

/* Text selection; x/y are viewport pixels. */
void view_select_start(View *v, float x, float y);
void view_select_extend(View *v, float x, float y);
void view_select_all(View *v);
void view_select_clear(View *v);
bool view_has_selection(const View *v);
/* Selected text as UTF-8 (SDL_free it), NULL when nothing is selected. */
char *view_selection_text(View *v);
/* The selection as an HTML fragment (headings, lists, emphasis, links,
 * code with its colours, tables), for rich-text paste. SDL_free it. */
char *view_selection_html(View *v);
/* Start of the selection (the current search match). */
SelPos view_selection_start(const View *v);

/* Search: the first visible text position, and a search for q from `from`
 * forwards or backwards, wrapping around. A match becomes the selection and
 * is scrolled into view. Case-insensitive unless q has a capital letter. */
SelPos view_top_pos(View *v);
bool view_search(View *v, const char *q, SelPos from, bool forward, bool *wrapped);

/* Centered two-line message, used for the welcome and error screens. */
void view_draw_message(View *v, SDL_Renderer *r, const char *title, const char *body);

#endif
