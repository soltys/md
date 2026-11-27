#ifndef MD_OUTLINE_H
#define MD_OUTLINE_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "theme.h"

/* The document's headings: the outline panel (Ctrl+Shift+T) and the
 * GitHub-style anchors that #links point at. */
typedef struct OutlineEntry {
    int block;            /* index in Document.blocks */
    int level;            /* 1-6 */
    char *title;          /* display text, one line */
    char *slug;           /* GitHub anchor: "Hello, World!" -> "hello-world" */
    TTF_Text *text;       /* drawn title (ellipsized to fit), made lazily */
} OutlineEntry;

typedef struct Outline {
    const Document *doc;  /* built for this document */
    OutlineEntry *entries;
    int count;
    int min_level;        /* the shallowest heading, drawn without indent */
    bool visible;
    float scroll;         /* pixels */
    int hover;            /* entry under the mouse, -1 = none */
    int current;          /* entry of the section on screen, -1 = none */
    int shown_current;    /* current when last drawn (to keep it in view) */
    /* What the cached texts were made for. */
    TTF_Font *text_font;
    float text_w;
    TTF_Text *header;
} Outline;

/* Rebuilds the entries for a new document (the panel keeps its scroll
 * position, so the live preview does not make it jump). */
void outline_sync(Outline *o, const Document *doc);
void outline_free(Outline *o);

/* Block index of the heading with this anchor ("#" optional, percent
 * escapes and case ignored), -1 if there is none. */
int outline_find(const Outline *o, const char *anchor);

/* The entry whose section contains source line `line`, -1 before the first
 * heading. */
int outline_entry_at_line(const Outline *o, int line);

/* Width of the panel for a window area this wide (0 when hidden). */
float outline_width(const Outline *o, float area_w, float scale);

/* rect is in window pixels. */
void outline_draw(Outline *o, SDL_Renderer *r, TTF_TextEngine *engine, const SDL_FRect *rect,
                  const Theme *th, float scale);
/* Entry at window pixel (x, y) inside rect, -1 for none. */
int outline_hit(const Outline *o, const SDL_FRect *rect, float x, float y, float scale);
/* Returns true when the hovered entry changed. */
bool outline_set_hover(Outline *o, int entry);
void outline_scroll_by(Outline *o, const SDL_FRect *rect, float dy, float scale);

/* Theme or fonts changed: texts are made again on the next draw. */
void outline_drop_texts(Outline *o);

#endif
