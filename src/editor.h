#ifndef MD_EDITOR_H
#define MD_EDITOR_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "nvim.h"

/* Draws Neovim's grid inside a rectangle of the window. */
typedef struct EdRun {
    TTF_Text *text;
    float x;
} EdRun;

typedef struct EdRow {
    EdRun *runs;
    int count, cap;
} EdRow;

typedef struct Editor {
    TTF_TextEngine *engine;
    Nvim *nv;
    float scale;
    float cell_w, cell_h, ascent;
    float px;
    EdRow *rows;
    int row_count;
    TTF_Text *cursor_text;
} Editor;

void editor_init(Editor *ed, TTF_TextEngine *engine, Nvim *nv);
void editor_set_scale(Editor *ed, float scale);
/* Grid size that fits in w x h pixels. */
void editor_grid_size(const Editor *ed, float w, float h, int *cols, int *rows);
void editor_draw(Editor *ed, SDL_Renderer *r, const SDL_FRect *rect, bool focused);
/* Pixel position inside the pane -> grid cell. */
void editor_cell_at(const Editor *ed, float x, float y, int *row, int *col);
void editor_destroy(Editor *ed);

#endif
