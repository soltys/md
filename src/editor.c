#include "editor.h"
#include "fonts.h"

#define EDITOR_PX 14.0f

static SDL_Color rgb(uint32_t c)
{
    SDL_Color k = {(Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c, 0xFF};
    return k;
}

static void set_color(SDL_Renderer *r, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void hl_colors(const Nvim *nv, int hl, uint32_t *fg, uint32_t *bg, uint32_t *sp)
{
    const NvHl *h = nvim_hl(nv, hl);
    uint32_t f = h->has_fg ? h->fg : nv->def_fg;
    uint32_t b = h->has_bg ? h->bg : nv->def_bg;
    if (h->reverse) { uint32_t t = f; f = b; b = t; }
    *fg = f;
    *bg = b;
    *sp = h->has_sp ? h->sp : f;
}

static TTF_Font *hl_font(const Editor *ed, int hl)
{
    const NvHl *h = nvim_hl(ed->nv, hl);
    FontFace face = h->bold ? (h->italic ? FONT_MONO_BOLD_ITALIC : FONT_MONO_BOLD)
                            : (h->italic ? FONT_MONO_ITALIC : FONT_MONO);
    return fonts_get(face, ed->px);
}

static void clear_row(EdRow *row)
{
    for (int i = 0; i < row->count; ++i) TTF_DestroyText(row->runs[i].text);
    row->count = 0;
}

static void free_rows(Editor *ed)
{
    for (int i = 0; i < ed->row_count; ++i) {
        clear_row(&ed->rows[i]);
        SDL_free(ed->rows[i].runs);
    }
    SDL_free(ed->rows);
    ed->rows = NULL;
    ed->row_count = 0;
}

void editor_init(Editor *ed, TTF_TextEngine *engine, Nvim *nv)
{
    SDL_zerop(ed);
    ed->engine = engine;
    ed->nv = nv;
    editor_set_scale(ed, 1.0f);
}

void editor_set_scale(Editor *ed, float scale)
{
    if (scale == ed->scale && ed->cell_w > 0) return;
    ed->scale = scale;
    ed->px = SDL_roundf(EDITOR_PX * scale);
    TTF_Font *f = fonts_get(FONT_MONO, ed->px);
    int adv = 0;
    if (f && TTF_GetGlyphMetrics(f, 'M', NULL, NULL, NULL, NULL, &adv) && adv > 0) {
        ed->cell_w = (float)adv;
    } else {
        ed->cell_w = SDL_roundf(ed->px * 0.6f);
    }
    ed->cell_h = f ? (float)TTF_GetFontLineSkip(f) : SDL_roundf(ed->px * 1.3f);
    ed->ascent = f ? (float)TTF_GetFontAscent(f) : ed->px;
    free_rows(ed); /* rebuild every run at the new size */
}

void editor_grid_size(const Editor *ed, float w, float h, int *cols, int *rows)
{
    *cols = SDL_max(1, (int)(w / ed->cell_w));
    *rows = SDL_max(1, (int)(h / ed->cell_h));
}

void editor_cell_at(const Editor *ed, float x, float y, int *row, int *col)
{
    *col = SDL_clamp((int)(x / ed->cell_w), 0, SDL_max(ed->nv->cols - 1, 0));
    *row = SDL_clamp((int)(y / ed->cell_h), 0, SDL_max(ed->nv->rows - 1, 0));
}

static void add_run(Editor *ed, EdRow *row, const char *s, size_t n, int hl, float x)
{
    if (n == 0) return;
    /* Skip runs that are only spaces; the background pass already drew them. */
    bool blank = true;
    for (size_t i = 0; i < n; ++i) if (s[i] != ' ') { blank = false; break; }
    if (blank) return;

    uint32_t fg, bg, sp;
    hl_colors(ed->nv, hl, &fg, &bg, &sp);
    SDL_Color c = rgb(fg);
    TTF_Text *t = TTF_CreateText(ed->engine, hl_font(ed, hl), s, n);
    if (!t) return;
    TTF_SetTextColor(t, c.r, c.g, c.b, 0xFF);
    if (row->count == row->cap) {
        row->cap = row->cap ? row->cap * 2 : 8;
        row->runs = SDL_realloc(row->runs, sizeof(EdRun) * (size_t)row->cap);
    }
    row->runs[row->count].text = t;
    row->runs[row->count].x = x;
    row->count++;
}

/* Splits a grid row into text runs. A run breaks on a highlight change and
 * around any non-ASCII cell, which is drawn on its own at its cell position
 * so glyphs from fallback fonts (emoji, CJK) can never shift the columns. */
static void build_row(Editor *ed, int r)
{
    Nvim *nv = ed->nv;
    EdRow *row = &ed->rows[r];
    clear_row(row);

    char buf[1024];
    size_t n = 0;
    int run_hl = -1;
    int run_col = 0;
    for (int c = 0; c < nv->cols; ++c) {
        const NvCell *x = &nv->cells[(size_t)r * (size_t)nv->cols + (size_t)c];
        if (x->len == 0) continue; /* right half of a wide char */
        bool ascii = x->len == 1 && (unsigned char)x->text[0] < 0x80;
        if (!ascii || x->hl != run_hl || n + x->len >= sizeof(buf)) {
            add_run(ed, row, buf, n, run_hl, (float)run_col * ed->cell_w);
            n = 0;
            run_hl = x->hl;
            run_col = c;
        }
        if (!ascii) {
            add_run(ed, row, x->text, x->len, x->hl, (float)c * ed->cell_w);
            run_hl = -1;
            continue;
        }
        buf[n++] = x->text[0];
    }
    add_run(ed, row, buf, n, run_hl, (float)run_col * ed->cell_w);
}

void editor_draw(Editor *ed, SDL_Renderer *r, const SDL_FRect *rect, bool focused)
{
    Nvim *nv = ed->nv;
    set_color(r, rgb(nv->def_bg));
    SDL_RenderFillRect(r, rect);
    if (!nv->cells) return;

    if (ed->row_count != nv->rows) {
        free_rows(ed);
        ed->rows = SDL_calloc((size_t)nv->rows, sizeof(EdRow));
        ed->row_count = nv->rows;
        for (int i = 0; i < nv->rows; ++i) nv->row_dirty[i] = true;
    }
    if (nv->colors_changed) {
        for (int i = 0; i < nv->rows; ++i) nv->row_dirty[i] = true;
        nv->colors_changed = false;
    }

    const float ox = rect->x, oy = rect->y;
    const float cw = ed->cell_w, ch = ed->cell_h;

    for (int row = 0; row < nv->rows; ++row) {
        if (nv->row_dirty[row]) {
            build_row(ed, row);
            nv->row_dirty[row] = false;
        }
        float y = oy + (float)row * ch;
        if (y > rect->y + rect->h) break;

        /* Backgrounds and decorations, merged per highlight run. */
        int c = 0;
        while (c < nv->cols) {
            const NvCell *x = &nv->cells[(size_t)row * (size_t)nv->cols + (size_t)c];
            int hl = x->hl;
            int end = c + 1;
            while (end < nv->cols && nv->cells[(size_t)row * (size_t)nv->cols + (size_t)end].hl == hl) end++;
            uint32_t fg, bg, sp;
            hl_colors(nv, hl, &fg, &bg, &sp);
            float x0 = ox + (float)c * cw, w = (float)(end - c) * cw;
            if (bg != nv->def_bg) {
                SDL_FRect box = {x0, y, w, ch};
                set_color(r, rgb(bg));
                SDL_RenderFillRect(r, &box);
            }
            const NvHl *h = nvim_hl(nv, hl);
            if (h->underline || h->undercurl) {
                SDL_FRect u = {x0, y + ed->ascent + 2, w, SDL_max(1.0f, SDL_floorf(ed->scale))};
                set_color(r, rgb(sp));
                SDL_RenderFillRect(r, &u);
            }
            if (h->strikethrough) {
                SDL_FRect s = {x0, y + ch * 0.55f, w, SDL_max(1.0f, SDL_floorf(ed->scale))};
                set_color(r, rgb(fg));
                SDL_RenderFillRect(r, &s);
            }
            c = end;
        }

        EdRow *er = &ed->rows[row];
        for (int i = 0; i < er->count; ++i) TTF_DrawRendererText(er->runs[i].text, ox + er->runs[i].x, y);
    }

    /* Cursor. */
    if (focused && !nv->busy && nv->cursor_row >= 0 && nv->cursor_row < nv->rows &&
        nv->cursor_col >= 0 && nv->cursor_col < nv->cols) {
        NvModeInfo mi = {NV_CURSOR_BLOCK, 100, 0};
        if (nv->mode_idx >= 0 && nv->mode_idx < nv->mode_count) mi = nv->modes[nv->mode_idx];
        const NvCell *x = &nv->cells[(size_t)nv->cursor_row * (size_t)nv->cols + (size_t)nv->cursor_col];
        uint32_t fg, bg, sp;
        hl_colors(nv, x->hl, &fg, &bg, &sp);
        uint32_t cur_bg = fg, cur_fg = bg;
        if (mi.attr_id > 0) {
            const NvHl *ch_hl = nvim_hl(nv, mi.attr_id);
            if (ch_hl->has_bg) cur_bg = ch_hl->reverse ? (ch_hl->has_fg ? ch_hl->fg : fg) : ch_hl->bg;
            if (ch_hl->has_fg) cur_fg = ch_hl->reverse ? ch_hl->bg : ch_hl->fg;
        }
        bool wide = nv->cursor_col + 1 < nv->cols &&
                    nv->cells[(size_t)nv->cursor_row * (size_t)nv->cols + (size_t)nv->cursor_col + 1].len == 0;
        float x0 = ox + (float)nv->cursor_col * cw, y0 = oy + (float)nv->cursor_row * ch;
        float w = wide ? 2 * cw : cw;
        float pct = (float)SDL_clamp(mi.percentage ? mi.percentage : 100, 1, 100) / 100.0f;
        SDL_FRect box;
        if (mi.shape == NV_CURSOR_VERTICAL) box = (SDL_FRect){x0, y0, SDL_max(1.0f, SDL_ceilf(w * pct)), ch};
        else if (mi.shape == NV_CURSOR_HORIZONTAL) box = (SDL_FRect){x0, y0 + ch - SDL_max(1.0f, SDL_ceilf(ch * pct)), w, SDL_max(1.0f, SDL_ceilf(ch * pct))};
        else box = (SDL_FRect){x0, y0, w, ch};
        set_color(r, rgb(cur_bg));
        SDL_RenderFillRect(r, &box);
        if (mi.shape == NV_CURSOR_BLOCK && x->len > 0 && !(x->len == 1 && x->text[0] == ' ')) {
            TTF_Font *f = hl_font(ed, x->hl);
            if (!ed->cursor_text) ed->cursor_text = TTF_CreateText(ed->engine, f, x->text, x->len);
            else {
                TTF_SetTextFont(ed->cursor_text, f);
                TTF_SetTextString(ed->cursor_text, x->text, x->len);
            }
            SDL_Color k = rgb(cur_fg);
            TTF_SetTextColor(ed->cursor_text, k.r, k.g, k.b, 0xFF);
            TTF_DrawRendererText(ed->cursor_text, x0, y0);
        }
    }
}

void editor_destroy(Editor *ed)
{
    free_rows(ed);
    if (ed->cursor_text) TTF_DestroyText(ed->cursor_text);
    ed->cursor_text = NULL;
}
