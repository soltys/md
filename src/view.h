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
    float x, y, w, h;
    float ax, ay;             /* absolute document position (selection) */
    int item;                 /* owning item index */
    uint32_t str, len;        /* absolute offset into Document.strings */
    TTF_Font *font;
    TTF_Text *text;
    SDL_Color color;
    uint16_t style;
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
void view_scroll_to(View *v, float y);
void view_scroll_to_end(View *v);
/* Scrolls so the block containing source line `line` (1-based) is on top. */
void view_scroll_to_line(View *v, int line);
/* The source line shown at the top of the viewport (1-based). */
int view_top_line(const View *v);
float view_line_height(const View *v);

/* Link under a viewport position (pixels); returns false if none. */
bool view_link_at(View *v, float x, float y, const char **url, size_t *len);

void view_draw(View *v, SDL_Renderer *r);

/* Text selection; x/y are viewport pixels. */
void view_select_start(View *v, float x, float y);
void view_select_extend(View *v, float x, float y);
void view_select_all(View *v);
void view_select_clear(View *v);
bool view_has_selection(const View *v);
/* Selected text as UTF-8 (SDL_free it), NULL when nothing is selected. */
char *view_selection_text(View *v);
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
