#ifndef MD_VIEW_H
#define MD_VIEW_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "theme.h"

typedef struct ViewItem {
    const Block *block;
    TTF_Text *text;
    TTF_Text *marker;  /* list bullet / number */
    TTF_Text *lang;    /* code block language label */
    float x, y, w, h;  /* text box, document coordinates (pixels) */
    float box_y, box_h;/* background box for code blocks */
} ViewItem;

typedef struct View {
    TTF_TextEngine *engine;
    Theme theme;
    Document *doc;
    ViewItem *items;
    int item_count;

    float scale;       /* display scale, 1.0 = 96 DPI */
    float width, height;
    float scroll;
    float content_height;

    TTF_Text *msg_title;
    TTF_Text *msg_body;
} View;

void view_init(View *v, TTF_TextEngine *engine, const Theme *theme);
void view_destroy(View *v);

/* Takes ownership of doc (may be NULL). */
void view_set_document(View *v, Document *doc);

/* Recomputes layout for the given output size (pixels) and display scale. */
void view_layout(View *v, float width, float height, float scale);

void view_scroll_by(View *v, float dy);
void view_scroll_to(View *v, float y);
float view_line_height(const View *v);

void view_draw(View *v, SDL_Renderer *r);

/* Centered two-line message, used for the welcome and error screens. */
void view_draw_message(View *v, SDL_Renderer *r, const char *title, const char *body);

#endif
