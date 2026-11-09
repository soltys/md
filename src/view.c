#include "view.h"
#include "emoji.h"
#include "fonts.h"

#define BODY_PX       16.0f
#define MAX_WIDTH     920.0f
#define MARGIN        32.0f
#define LIST_INDENT   26.0f
#define QUOTE_INDENT  18.0f
#define CODE_PAD      14.0f
#define CELL_PAD_X    12.0f
#define CELL_PAD_Y    7.0f
#define LINE_FACTOR   1.45f

static const float heading_scale[7] = {1.0f, 2.0f, 1.5f, 1.25f, 1.0f, 0.875f, 0.85f};

static void set_color(SDL_Renderer *r, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static bool is_boxed(const Block *b)
{
    return b->kind == BLOCK_CODE || b->kind == BLOCK_HTML || b->kind == BLOCK_TABLE;
}

void view_draw_selection(View *v, SDL_Renderer *r);

/* ------------------------------------------------------------ layout -- */

typedef struct TextStyle {
    FontFace face;            /* base face (FONT_REGULAR, FONT_BOLD for headings, FONT_MONO for code) */
    float px;
    SDL_Color color;
    bool pre;                 /* keep whitespace, break only at newlines */
    float line_factor;
} TextStyle;

typedef struct Tok {
    uint32_t start, len;      /* relative to text */
    TTF_Font *font;
    uint16_t style;
    uint8_t color_class;
    uint8_t kind;             /* TOK_* */
    uint32_t url, url_len;
    Image *img;
    float w, h, ascent, lineskip;
} Tok;

enum { TOK_WORD, TOK_SPACE, TOK_NEWLINE, TOK_IMAGE };

static Tok *toks;
static int tok_count, tok_cap;
static int layout_item_index; /* item being laid out, stamped on fragments */

static FontFace styled_face(FontFace base, uint16_t style)
{
    if (style & ST_CODE) return FONT_MONO;
    bool bold = (style & ST_BOLD) || base == FONT_BOLD || base == FONT_BOLD_ITALIC;
    bool italic = (style & ST_ITALIC) || base == FONT_ITALIC || base == FONT_BOLD_ITALIC;
    if (base == FONT_MONO || base == FONT_MONO_ITALIC || base == FONT_MONO_BOLD ||
        base == FONT_MONO_BOLD_ITALIC) {
        return bold ? (italic ? FONT_MONO_BOLD_ITALIC : FONT_MONO_BOLD)
                    : (italic ? FONT_MONO_ITALIC : FONT_MONO);
    }
    return bold ? (italic ? FONT_BOLD_ITALIC : FONT_BOLD) : (italic ? FONT_ITALIC : base);
}

static void push_tok(const Tok *t)
{
    if (tok_count == tok_cap) {
        tok_cap = tok_cap ? tok_cap * 2 : 1024;
        toks = SDL_realloc(toks, sizeof(Tok) * (size_t)tok_cap);
    }
    toks[tok_count++] = *t;
}

static void measure(Tok *t, const char *text)
{
    int w = 0, h = 0;
    if (t->len) TTF_GetStringSize(t->font, text + t->start, t->len, &w, &h);
    t->w = (float)w;
    t->ascent = (float)TTF_GetFontAscent(t->font);
    t->lineskip = (float)TTF_GetFontLineSkip(t->font);
    t->h = t->lineskip;
}

/* Splits a styled segment [a, b) into tokens. */
static void tokenize(View *v, const char *text, uint32_t a, uint32_t b, const Span *sp,
                     const TextStyle *ts, const char *doc_path)
{
    uint16_t style = sp ? sp->style : 0;
    float px = ts->px * ((style & ST_CODE) && !ts->pre ? 0.875f : 1.0f);
    TTF_Font *font = fonts_get(styled_face(ts->face, style), px);
    TTF_Font *emoji_font = fonts_get_emoji(px);

    Tok base;
    SDL_zero(base);
    base.style = style;
    base.color_class = sp ? sp->color : 0;
    base.url = sp ? sp->url : 0;
    base.url_len = sp ? sp->url_len : 0;

    if ((style & ST_IMAGE) && sp && sp->url_len) {
        Image *img = images_get(doc_path, document_str(v->doc, sp->url), sp->url_len);
        if (img->state == IMAGE_READY) {
            Tok t = base;
            t.kind = TOK_IMAGE;
            t.start = a;
            t.len = b - a;
            t.img = img;
            t.font = font;
            t.w = (float)img->w * v->scale;
            t.h = (float)img->h * v->scale;
            t.ascent = t.h;
            t.lineskip = t.h;
            push_tok(&t);
            return;
        }
        /* Loading or failed: show the alt text. */
        base.style = (uint16_t)((style & ~ST_IMAGE) | ST_ITALIC);
        font = fonts_get(styled_face(ts->face, base.style), px);
        base.img = img;
    }

    uint32_t i = a;
    while (i < b) {
        Tok t = base;
        t.start = i;
        t.font = font;
        char c = text[i];
        if (c == '\n') {
            t.kind = TOK_NEWLINE;
            t.len = 1;
            t.font = font;
            t.w = 0;
            t.ascent = (float)TTF_GetFontAscent(font);
            t.lineskip = (float)TTF_GetFontLineSkip(font);
            t.h = t.lineskip;
            push_tok(&t);
            i++;
            continue;
        }
        size_t em = emoji_cluster(text + i, b - i);
        if (em) {
            uint32_t cp;
            utf8_decode(text + i, b - i, &cp);
            if (emoji_font && TTF_FontHasGlyph(emoji_font, cp)) {
                t.kind = TOK_WORD;
                t.len = (uint32_t)em;
                t.font = emoji_font;
                measure(&t, text);
                push_tok(&t);
                i += (uint32_t)em;
                continue;
            }
        }
        if (!ts->pre && (c == ' ' || c == '\t')) {
            uint32_t j = i;
            while (j < b && (text[j] == ' ' || text[j] == '\t')) j++;
            t.kind = TOK_SPACE;
            t.len = j - i;
            measure(&t, text);
            push_tok(&t);
            i = j;
            continue;
        }
        /* Word: up to whitespace, newline or an emoji. In pre mode spaces
         * belong to the word. */
        uint32_t j = i;
        while (j < b) {
            char d = text[j];
            if (d == '\n') break;
            if (!ts->pre && (d == ' ' || d == '\t')) break;
            if ((unsigned char)d >= 0x80 && j > i && emoji_cluster(text + j, b - j)) break;
            j++;
        }
        if (j == i) j = i + 1;
        t.kind = TOK_WORD;
        t.len = j - i;
        measure(&t, text);
        push_tok(&t);
        i = j;
    }
}

static SDL_Color tok_color(View *v, const Tok *t, const TextStyle *ts)
{
    if (t->color_class && t->color_class < HL_COUNT) return v->theme.syntax[t->color_class];
    if (t->style & ST_LINK) return v->theme.accent;
    if (t->img) return v->theme.muted;
    return ts->color;
}

static void push_frag(View *v, const Frag *f)
{
    if (v->frag_count == v->frag_cap) {
        v->frag_cap = v->frag_cap ? v->frag_cap * 2 : 4096;
        v->frags = SDL_realloc(v->frags, sizeof(Frag) * (size_t)v->frag_cap);
    }
    v->frags[v->frag_count++] = *f;
}

/* Lays out styled text into `width`. With out == NULL only measures and
 * returns the widest line (used for table column widths). */
static float layout_text(View *v, Para *out, uint32_t text_off, uint32_t len,
                         const Span *spans, uint32_t span_count, float width,
                         const TextStyle *ts)
{
    const char *text = v->doc->strings + text_off;
    const char *doc_path = v->doc->path;
    tok_count = 0;

    /* Segments: gaps between spans use the default style. */
    uint32_t pos = 0;
    for (uint32_t i = 0; i < span_count; ++i) {
        const Span *sp = &spans[i];
        if (sp->len == 0) continue;
        if (sp->start > pos) tokenize(v, text, pos, sp->start, NULL, ts, doc_path);
        tokenize(v, text, sp->start, sp->start + sp->len, sp, ts, doc_path);
        pos = sp->start + sp->len;
    }
    if (pos < len) tokenize(v, text, pos, len, NULL, ts, doc_path);

    if (out) {
        out->first = v->frag_count;
        out->count = 0;
        out->w = out->h = 0;
    }

    float max_w = 0;
    float y = 0;
    int i = 0;
    if (tok_count == 0 && out) {
        TTF_Font *f = fonts_get(ts->face, ts->px);
        out->h = SDL_roundf((float)TTF_GetFontLineSkip(f) * ts->line_factor);
        return 0;
    }
    while (i < tok_count) {
        /* Collect one line: tokens [i, end). */
        float x = 0;
        int end = i;
        while (end < tok_count && toks[end].kind == TOK_SPACE && !ts->pre) end++; /* skip leading spaces */
        int line_start = end;
        int last_break = -1;
        while (end < tok_count) {
            Tok *t = &toks[end];
            if (t->kind == TOK_NEWLINE) break;
            if (t->kind == TOK_SPACE) last_break = end;
            if (!ts->pre && x + t->w > width && end > line_start && t->kind != TOK_SPACE) {
                /* wrap: at the last space if there is one on this line */
                if (last_break > line_start) end = last_break;
                break;
            }
            x += t->w;
            end++;
        }
        /* Metrics of the line. */
        float asc = 0, skip = 0;
        for (int k = line_start; k < end; ++k) {
            if (toks[k].kind == TOK_IMAGE) {
                asc = SDL_max(asc, toks[k].h);
                skip = SDL_max(skip, toks[k].h);
            } else {
                asc = SDL_max(asc, toks[k].ascent);
                skip = SDL_max(skip, toks[k].lineskip);
            }
        }
        if (skip == 0) { /* empty line */
            Tok *t = &toks[SDL_min(end, tok_count - 1)];
            asc = t->ascent;
            skip = t->lineskip;
        }
        bool has_image = false;
        for (int k = line_start; k < end; ++k) if (toks[k].kind == TOK_IMAGE) has_image = true;
        float line_h = has_image ? skip : SDL_roundf(skip * ts->line_factor);
        float base = has_image ? asc : SDL_roundf((line_h - skip) * 0.5f + asc);

        /* Place fragments. */
        x = 0;
        Frag *cur = NULL;
        for (int k = line_start; k < end; ++k) {
            Tok *t = &toks[k];
            if (out) {
                float ty = y + base - (t->kind == TOK_IMAGE ? t->h : t->ascent);
                SDL_Color c = tok_color(v, t, ts);
                bool mergeable = cur && t->kind != TOK_IMAGE && !cur->img &&
                                 cur->font == t->font && cur->style == t->style &&
                                 cur->url == t->url && cur->str + cur->len == text_off + t->start &&
                                 SDL_memcmp(&cur->color, &c, sizeof(c)) == 0;
                if (mergeable) {
                    cur->len += t->len;
                    cur->w = x + t->w - cur->x;
                } else if (!(t->kind == TOK_SPACE && !(t->style & (ST_CODE | ST_LINK | ST_STRIKE)))) {
                    Frag f;
                    SDL_zero(f);
                    f.x = x;
                    f.y = ty;
                    f.w = t->w;
                    f.h = t->kind == TOK_IMAGE ? t->h : t->lineskip;
                    f.str = text_off + t->start;
                    f.len = t->len;
                    f.font = t->font;
                    f.color = c;
                    f.style = t->style;
                    f.url = t->url;
                    f.url_len = t->url_len;
                    f.img = t->kind == TOK_IMAGE ? t->img : NULL;
                    f.item = layout_item_index;
                    push_frag(v, &f);
                    cur = &v->frags[v->frag_count - 1];
                } else {
                    cur = NULL; /* plain spaces are not drawn */
                }
            }
            x += t->w;
        }
        /* Do not count trailing spaces in the width. */
        float w = x;
        for (int k = end - 1; k >= line_start && toks[k].kind == TOK_SPACE; --k) w -= toks[k].w;
        max_w = SDL_max(max_w, w);
        y += line_h;

        i = end;
        if (i < tok_count && toks[i].kind == TOK_NEWLINE) i++;
        else if (i < tok_count && toks[i].kind == TOK_SPACE) i++;
    }
    if (out) {
        out->count = v->frag_count - out->first;
        out->w = max_w;
        out->h = y;
    }
    return max_w;
}

static Para layout_para(View *v, uint32_t text, uint32_t len, uint32_t span_first,
                        uint32_t span_count, float width, const TextStyle *ts)
{
    Para p;
    SDL_zero(p);
    layout_text(v, &p, text, len, v->doc->spans + span_first, span_count, width, ts);
    return p;
}

static TextStyle body_style(const View *v, const Block *b)
{
    TextStyle ts;
    ts.face = FONT_REGULAR;
    ts.px = BODY_PX * v->scale;
    ts.color = b->quote_depth ? v->theme.muted : v->theme.text;
    ts.pre = false;
    ts.line_factor = LINE_FACTOR;
    if (b->kind == BLOCK_HEADING) {
        ts.face = b->level <= 4 ? FONT_BOLD : FONT_MEDIUM;
        ts.px = BODY_PX * v->scale * heading_scale[b->level <= 6 ? b->level : 6];
        ts.color = v->theme.heading;
        ts.line_factor = 1.25f;
    } else if (b->kind == BLOCK_CODE || b->kind == BLOCK_HTML) {
        ts.face = FONT_MONO;
        ts.px = BODY_PX * 0.875f * v->scale;
        ts.color = v->theme.code_text;
        ts.pre = true;
        ts.line_factor = 1.35f;
    }
    return ts;
}

static void layout_table(View *v, ViewItem *it, float width)
{
    const Block *b = it->block;
    const Document *doc = v->doc;
    const float s = v->scale;
    int cols = b->cols, rows = b->rows;
    if (cols == 0 || rows == 0) { it->h = 0; return; }

    SDL_free(it->cells);
    SDL_free(it->col_w);
    SDL_free(it->row_h);
    it->cells = SDL_calloc(b->cell_count ? b->cell_count : 1, sizeof(ViewCell));
    it->col_w = SDL_calloc((size_t)cols, sizeof(float));
    it->row_h = SDL_calloc((size_t)rows, sizeof(float));
    float *natural = SDL_calloc((size_t)cols, sizeof(float));

    TextStyle ts = body_style(v, b);
    TextStyle hs = ts;
    hs.face = FONT_BOLD;
    ts.line_factor = hs.line_factor = 1.3f;
    const float pad_x = CELL_PAD_X * s, pad_y = CELL_PAD_Y * s;

    for (uint32_t i = 0; i < b->cell_count; ++i) {
        const TableCell *c = &doc->cells[b->cell_first + i];
        float w = layout_text(v, NULL, c->text, c->text_len, doc->spans + c->span_first,
                              c->span_count, 1e9f, c->row == 0 ? &hs : &ts);
        natural[c->col] = SDL_max(natural[c->col], w + 2 * pad_x);
    }
    float total = 0;
    for (int c = 0; c < cols; ++c) {
        natural[c] = SDL_max(natural[c], 3 * pad_x);
        total += natural[c];
    }
    for (int c = 0; c < cols; ++c) {
        it->col_w[c] = total <= width ? natural[c] : SDL_floorf(natural[c] * width / total);
        it->col_w[c] = SDL_max(it->col_w[c], 2 * pad_x + 8 * s);
    }

    for (uint32_t i = 0; i < b->cell_count; ++i) {
        const TableCell *c = &doc->cells[b->cell_first + i];
        ViewCell *vc = &it->cells[i];
        vc->para = layout_para(v, c->text, c->text_len, c->span_first, c->span_count,
                               it->col_w[c->col] - 2 * pad_x, c->row == 0 ? &hs : &ts);
        it->row_h[c->row] = SDL_max(it->row_h[c->row], vc->para.h + 2 * pad_y);
    }
    float y = 0;
    float *row_y = SDL_calloc((size_t)rows, sizeof(float));
    for (int r = 0; r < rows; ++r) {
        if (it->row_h[r] == 0) it->row_h[r] = 2 * pad_y + BODY_PX * s;
        row_y[r] = y;
        y += it->row_h[r];
    }
    for (uint32_t i = 0; i < b->cell_count; ++i) {
        const TableCell *c = &doc->cells[b->cell_first + i];
        ViewCell *vc = &it->cells[i];
        float x = 0;
        for (int k = 0; k < c->col && k < cols; ++k) x += it->col_w[k];
        float inner = it->col_w[c->col] - 2 * pad_x;
        int align = table_align(b, c->col);
        float dx = align == ALIGN_RIGHT ? inner - vc->para.w : align == ALIGN_CENTER ? (inner - vc->para.w) * 0.5f : 0;
        vc->x = x + pad_x + SDL_max(0.0f, SDL_floorf(dx));
        vc->y = row_y[c->row] + pad_y;
    }
    float tw = 0;
    for (int c = 0; c < cols; ++c) tw += it->col_w[c];
    it->w = tw;
    it->h = y;
    SDL_free(row_y);
    SDL_free(natural);
}

static float content_x0(const View *v, float *content_w)
{
    const float s = v->scale;
    float cw = SDL_min(MAX_WIDTH * s, v->width - 2 * MARGIN * s);
    if (cw < 100 * s) cw = SDL_max(v->width - 16 * s, 50.0f);
    *content_w = cw;
    return SDL_floorf((v->width - cw) * 0.5f);
}

static void anchor_para(View *v, const Para *p, float ox, float oy)
{
    for (int i = p->first; i < p->first + p->count; ++i) {
        v->frags[i].ax = ox + v->frags[i].x;
        v->frags[i].ay = oy + v->frags[i].y;
    }
}

static void layout_item(View *v, int idx)
{
    ViewItem *it = &v->items[idx];
    layout_item_index = idx;
    const Block *b = it->block;
    const Document *doc = v->doc;
    const float s = v->scale;
    float content_w;
    const float x0 = content_x0(v, &content_w);
    float y = v->next_y;

    float indent = b->list_depth * LIST_INDENT * s + b->quote_depth * QUOTE_INDENT * s;
    float x = x0 + indent;
    float w = content_w - indent;
    if (b->kind == BLOCK_CODE || b->kind == BLOCK_HTML) {
        x += CODE_PAD * s;
        w -= 2 * CODE_PAD * s;
    }
    if (idx > 0 && b->kind == BLOCK_HEADING) y += 10 * s;

    TextStyle ts = body_style(v, b);
    it->para.count = it->marker.count = it->lang.count = 0;

    if (b->marker_len && b->task == TASK_NONE) {
        TextStyle ms = body_style(v, b);
        ms.color = v->theme.muted;
        if (b->kind != BLOCK_PARAGRAPH) { ms.face = FONT_REGULAR; ms.px = BODY_PX * s; }
        it->marker = layout_para(v, b->marker, b->marker_len, 0, 0, 1e9f, &ms);
    }
    if (b->lang_len) {
        TextStyle ls = {FONT_MEDIUM, BODY_PX * 0.75f * s, v->theme.muted, false, 1.0f};
        it->lang = layout_para(v, b->lang, b->lang_len, 0, 0, 1e9f, &ls);
    }

    float text_x = x, text_w = w;
    if (b->task != TASK_NONE) {
        text_x += 22 * s;
        text_w -= 22 * s;
    }

    it->x = text_x;
    it->w = text_w;

    switch (b->kind) {
    case BLOCK_RULE:
        y += 8 * s;
        it->y = y;
        it->h = SDL_max(2 * s, 1.0f);
        y += it->h + 24 * s;
        break;
    case BLOCK_CODE:
    case BLOCK_HTML: {
        it->para = layout_para(v, b->text, b->text_len, b->span_first, b->span_count, 1e9f, &ts);
        float label_h = it->lang.count ? it->lang.h + 4 * s : 0;
        it->box_y = y;
        it->y = y + CODE_PAD * s + label_h;
        it->h = it->para.h;
        it->box_h = it->h + 2 * CODE_PAD * s + label_h;
        y += it->box_h + 16 * s;
        break;
    }
    case BLOCK_TABLE:
        it->x = x;
        layout_table(v, it, w);
        it->box_y = y;
        it->y = y;
        it->box_h = it->h;
        y += it->h + 16 * s;
        break;
    case BLOCK_HEADING:
        it->para = layout_para(v, b->text, b->text_len, b->span_first, b->span_count, text_w, &ts);
        it->y = y;
        it->h = it->para.h;
        y += it->h + (b->level <= 2 ? 22 * s : 10 * s);
        break;
    default: {
        it->para = layout_para(v, b->text, b->text_len, b->span_first, b->span_count, text_w, &ts);
        it->y = y;
        it->h = SDL_max(it->para.h, it->marker.h);
        bool tight = b->list_depth > 0 && idx + 1 < v->item_count &&
                     v->items[idx + 1].block->list_depth > 0;
        y += it->h + (tight ? 4 * s : 14 * s);
        break;
    }
    }
    (void)doc;
    /* Absolute positions for hit testing / selection. */
    anchor_para(v, &it->para, it->x, it->y);
    anchor_para(v, &it->marker, it->x - it->marker.w - 8 * s, it->y);
    if (b->kind == BLOCK_TABLE) {
        for (uint32_t c = 0; c < b->cell_count; ++c) {
            anchor_para(v, &it->cells[c].para, it->x + it->cells[c].x, it->y + it->cells[c].y);
        }
    }
    v->next_y = y;
    v->content_height = y + MARGIN * s;
}

static void destroy_texts(View *v)
{
    v->has_sel = false;
    for (int i = 0; i < v->frag_count; ++i) {
        if (v->frags[i].text) TTF_DestroyText(v->frags[i].text);
    }
    v->frag_count = 0;
}

static void free_items(View *v)
{
    destroy_texts(v);
    for (int i = 0; i < v->item_count; ++i) {
        SDL_free(v->items[i].cells);
        SDL_free(v->items[i].col_w);
        SDL_free(v->items[i].row_h);
    }
    SDL_free(v->items);
    v->items = NULL;
    v->item_count = 0;
    v->laid = 0;
}

static float item_top(const ViewItem *it)
{
    return is_boxed(it->block) ? it->box_y : it->y;
}

static void layout_until(View *v, int idx, float y_limit)
{
    while (v->laid < v->item_count && (v->laid <= idx || v->next_y < y_limit)) {
        layout_item(v, v->laid++);
    }
}

/* Re-lays out from scratch, keeping the first visible block in place. */
static void relayout_anchored(View *v)
{
    int anchor = -1;
    float offset = 0;
    for (int i = 0; i < v->laid; ++i) {
        const ViewItem *it = &v->items[i];
        float bottom = is_boxed(it->block) ? it->box_y + it->box_h : it->y + it->h;
        if (bottom > v->scroll) {
            anchor = i;
            offset = v->scroll - item_top(it);
            break;
        }
    }
    destroy_texts(v);
    v->laid = 0;
    v->next_y = MARGIN * v->scale;
    v->content_height = v->next_y;
    if (anchor >= 0) {
        layout_until(v, anchor, 0);
        v->scroll = item_top(&v->items[anchor]) + offset;
    } else {
        v->scroll = 0;
    }
    layout_until(v, -1, v->scroll + 2 * v->height);
    view_scroll_by(v, 0);
}

void view_init(View *v, TTF_TextEngine *engine, const Theme *theme)
{
    SDL_zerop(v);
    v->engine = engine;
    v->theme = *theme;
    v->scale = 1.0f;
}

void view_destroy(View *v)
{
    free_items(v);
    SDL_free(v->frags);
    v->frags = NULL;
    document_free(v->doc);
    if (v->msg_title) TTF_DestroyText(v->msg_title);
    if (v->msg_body) TTF_DestroyText(v->msg_body);
    v->doc = NULL;
}

void view_set_document(View *v, Document *doc, bool keep_scroll)
{
    float old_scroll = v->scroll;
    free_items(v);
    document_free(v->doc);
    v->doc = doc;
    v->scroll = 0;
    v->next_y = MARGIN * v->scale;
    v->content_height = v->next_y;
    if (!doc || doc->error) return;

    v->items = SDL_calloc((size_t)(doc->block_count ? doc->block_count : 1), sizeof(ViewItem));
    v->item_count = doc->block_count;
    for (int i = 0; i < doc->block_count; ++i) v->items[i].block = &doc->blocks[i];

    float target = keep_scroll ? old_scroll : 0;
    layout_until(v, -1, target + 2 * v->height);
    v->scroll = target;
    view_scroll_by(v, 0);
}

void view_layout(View *v, float width, float height, float scale)
{
    bool changed = width != v->width || scale != v->scale;
    v->width = width;
    v->height = height;
    if (scale != v->scale) {
        v->scroll = v->scroll * scale / v->scale;
        v->scale = scale;
    }
    if (changed && v->doc && !v->doc->error) relayout_anchored(v);
    else if (v->doc) {
        layout_until(v, -1, v->scroll + 2 * v->height);
        view_scroll_by(v, 0);
    }
}

void view_relayout(View *v)
{
    if (v->doc && !v->doc->error) relayout_anchored(v);
}

bool view_layout_pending(const View *v)
{
    return v->doc && v->laid < v->item_count;
}

bool view_layout_more(View *v, Uint64 budget_ms)
{
    Uint64 start = SDL_GetTicks();
    while (v->laid < v->item_count) {
        layout_item(v, v->laid++);
        if (SDL_GetTicks() - start >= budget_ms) break;
    }
    return v->laid < v->item_count;
}

float view_line_height(const View *v)
{
    return BODY_PX * 1.5f * v->scale;
}

void view_scroll_to(View *v, float y)
{
    /* Make sure what is about to be shown is laid out. */
    layout_until(v, -1, y + 2 * v->height);
    float max_scroll = SDL_max(0.0f, v->content_height - v->height);
    v->scroll = SDL_clamp(y, 0.0f, max_scroll);
}

void view_scroll_by(View *v, float dy)
{
    view_scroll_to(v, v->scroll + dy);
}

void view_scroll_to_end(View *v)
{
    layout_until(v, v->item_count, 0);
    view_scroll_to(v, v->content_height);
}

/* Source lines a block spans (at least 1). */
static int block_lines(const Document *doc, const Block *b)
{
    int n = 0;
    if (doc->source && b->src_end <= doc->source_len) {
        for (uint32_t i = b->src_start; i < b->src_end; ++i) n += doc->source[i] == '\n';
        /* A trailing newline ends the last line rather than starting one. */
        if (b->src_end > b->src_start && doc->source[b->src_end - 1] == '\n') --n;
    }
    return n + 1;
}

static float item_height(const ViewItem *it)
{
    return is_boxed(it->block) ? it->box_h : it->h;
}

/* Lines map to blocks; inside a block the position is interpolated, so long
 * code blocks and paragraphs scroll smoothly both ways. */
void view_scroll_to_line(View *v, int line)
{
    if (!v->doc || v->item_count == 0) return;
    int best = 0;
    for (int i = 0; i < v->item_count; ++i) {
        if ((int)v->items[i].block->src_line <= line) best = i;
        else break;
    }
    layout_until(v, best, 0);
    const ViewItem *it = &v->items[best];
    int lines = block_lines(v->doc, it->block);
    int into = line - (int)it->block->src_line;
    float frac = into <= 0 ? 0 : into >= lines ? 1 : (float)into / (float)lines;
    view_scroll_to(v, item_top(it) + frac * item_height(it) - MARGIN * v->scale);
}

int view_top_line(const View *v)
{
    if (!v->doc || v->laid == 0) return 1;
    float y = v->scroll + MARGIN * v->scale;
    const ViewItem *it = &v->items[0];
    for (int i = 0; i < v->laid; ++i) {
        if (item_top(&v->items[i]) <= y + 0.5f) it = &v->items[i];
        else break;
    }
    float h = item_height(it);
    int lines = block_lines(v->doc, it->block);
    float frac = h > 0 ? (y - item_top(it)) / h : 0;
    int into = frac <= 0 ? 0 : (int)(frac * (float)lines);
    if (into >= lines) into = lines; /* the gap after the block: next line */
    return (int)it->block->src_line + into;
}

/* -------------------------------------------------------------- draw -- */

static void draw_para(View *v, SDL_Renderer *r, const Para *p, float ox, float oy)
{
    const float s = v->scale;
    for (int i = p->first; i < p->first + p->count; ++i) {
        Frag *f = &v->frags[i];
        float x = ox + f->x, y = oy + f->y;
        if (f->img && f->img->state == IMAGE_READY) {
            SDL_FRect dst = {x, y, f->w, f->h};
            SDL_RenderTexture(r, f->img->texture, NULL, &dst);
            continue;
        }
        if (f->style & ST_CODE) {
            SDL_FRect bg = {x - 3 * s, y, f->w + 6 * s, f->h};
            set_color(r, v->theme.code_bg);
            SDL_RenderFillRect(r, &bg);
        }
        if (!f->text) {
            f->text = TTF_CreateText(v->engine, f->font, v->doc->strings + f->str, f->len);
            if (f->text) TTF_SetTextColor(f->text, f->color.r, f->color.g, f->color.b, f->color.a);
        }
        if (f->text) TTF_DrawRendererText(f->text, x, y);
        if (f->style & ST_STRIKE) {
            float asc = (float)TTF_GetFontAscent(f->font);
            SDL_FRect line = {x, y + asc * 0.68f, f->w, SDL_max(1.0f, SDL_floorf(s))};
            set_color(r, f->color);
            SDL_RenderFillRect(r, &line);
        }
        if (f->style & ST_LINK) {
            float asc = (float)TTF_GetFontAscent(f->font);
            SDL_FRect line = {x, y + asc + 2 * s, f->w, SDL_max(1.0f, SDL_floorf(s))};
            SDL_Color c = f->color;
            c.a = 0x60;
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            set_color(r, c);
            SDL_RenderFillRect(r, &line);
        }
    }
}

static void draw_checkbox(SDL_Renderer *r, const Theme *th, float x, float y, float size,
                          bool checked, float s)
{
    SDL_FRect box = {x, y, size, size};
    if (checked) {
        set_color(r, th->accent);
        SDL_RenderFillRect(r, &box);
        set_color(r, th->bg);
        for (float t = 0; t < 1.5f * s; t += 0.5f) {
            SDL_RenderLine(r, x + size * 0.22f, y + size * 0.52f + t, x + size * 0.42f, y + size * 0.72f + t);
            SDL_RenderLine(r, x + size * 0.42f, y + size * 0.72f + t, x + size * 0.78f, y + size * 0.30f + t);
        }
    } else {
        set_color(r, th->muted);
        SDL_RenderRect(r, &box);
    }
}

static void draw_table(View *v, SDL_Renderer *r, ViewItem *it, float dy)
{
    const Block *b = it->block;
    const Theme *th = &v->theme;
    const float s = v->scale;
    float x0 = it->x, y0 = it->y + dy;
    float line = SDL_max(1.0f, SDL_floorf(s));

    /* Row backgrounds. */
    float y = y0;
    for (int row = 0; row < b->rows; ++row) {
        if (row == 0 || row % 2 == 0) {
            SDL_FRect bg = {x0, y, it->w, it->row_h[row]};
            set_color(r, th->code_bg);
            if (row != 0 || true) SDL_RenderFillRect(r, &bg);
        }
        y += it->row_h[row];
    }
    /* Grid lines. */
    set_color(r, th->rule);
    y = y0;
    for (int row = 0; row <= b->rows; ++row) {
        SDL_FRect l = {x0, y, it->w, line};
        SDL_RenderFillRect(r, &l);
        if (row < b->rows) y += it->row_h[row];
    }
    float x = x0;
    for (int c = 0; c <= b->cols; ++c) {
        SDL_FRect l = {x, y0, line, it->h};
        SDL_RenderFillRect(r, &l);
        if (c < b->cols) x += it->col_w[c];
    }
    for (uint32_t i = 0; i < b->cell_count; ++i) {
        ViewCell *vc = &it->cells[i];
        draw_para(v, r, &vc->para, x0 + vc->x, y0 + vc->y);
    }
}

void view_draw(View *v, SDL_Renderer *r)
{
    const Theme *th = &v->theme;
    const float s = v->scale;
    const float top = v->scroll;
    const float bottom = v->scroll + v->height;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < v->laid; ++i) {
        ViewItem *it = &v->items[i];
        const Block *b = it->block;
        float y0 = item_top(it);
        float y1 = is_boxed(b) ? it->box_y + it->box_h : it->y + it->h;
        if (y1 < top) continue;
        if (y0 > bottom) break;
        float dy = -v->scroll;

        for (int q = 0; q < b->quote_depth; ++q) {
            float qx = SDL_floorf((v->width - SDL_min(MAX_WIDTH * s, v->width - 2 * MARGIN * s)) * 0.5f)
                       + b->list_depth * LIST_INDENT * s + q * QUOTE_INDENT * s;
            SDL_FRect bar = {qx, y0 + dy - 4 * s, 4 * s, (y1 - y0) + 8 * s + 10 * s};
            set_color(r, th->quote_bar);
            SDL_RenderFillRect(r, &bar);
        }

        switch (b->kind) {
        case BLOCK_RULE: {
            SDL_FRect l = {it->x, it->y + dy, it->w, it->h};
            set_color(r, th->rule);
            SDL_RenderFillRect(r, &l);
            break;
        }
        case BLOCK_CODE:
        case BLOCK_HTML: {
            float pad = CODE_PAD * s;
            SDL_FRect box = {it->x - pad, it->box_y + dy, it->w + 2 * pad, it->box_h};
            set_color(r, th->code_bg);
            SDL_RenderFillRect(r, &box);
            if (it->lang.count) draw_para(v, r, &it->lang, it->x, it->box_y + dy + pad * 0.6f);
            /* Long lines are clipped to the box (no wrapping in code). */
            SDL_Rect clip = {(int)box.x, (int)box.y, (int)box.w, (int)box.h};
            SDL_SetRenderClipRect(r, &clip);
            draw_para(v, r, &it->para, it->x, it->y + dy);
            SDL_SetRenderClipRect(r, NULL);
            continue;
        }
        case BLOCK_TABLE:
            draw_table(v, r, it, dy);
            continue;
        case BLOCK_HEADING:
            if (b->level <= 2) {
                SDL_FRect l = {it->x, it->y + it->h + dy + 8 * s, it->w, SDL_max(1.0f, s)};
                set_color(r, th->rule);
                SDL_RenderFillRect(r, &l);
            }
            break;
        default:
            break;
        }

        if (it->marker.count) {
            draw_para(v, r, &it->marker, it->x - it->marker.w - 8 * s, it->y + dy);
        }
        if (b->task != TASK_NONE) {
            float size = 14 * s;
            float line_h = BODY_PX * s * LINE_FACTOR;
            draw_checkbox(r, th, it->x - 22 * s, it->y + dy + (line_h - size) * 0.5f, size,
                          b->task == TASK_CHECKED, s);
        }
        draw_para(v, r, &it->para, it->x, it->y + dy);
    }
    view_draw_selection(v, r);
}

static bool para_link_at(View *v, const Para *p, float ox, float oy, float x, float y,
                         const char **url, size_t *len)
{
    for (int i = p->first; i < p->first + p->count; ++i) {
        const Frag *f = &v->frags[i];
        if (!f->url_len || !(f->style & ST_LINK)) continue;
        if (x >= ox + f->x && x < ox + f->x + f->w && y >= oy + f->y && y < oy + f->y + f->h) {
            *url = v->doc->strings + f->url;
            *len = f->url_len;
            return true;
        }
    }
    return false;
}

bool view_link_at(View *v, float x, float y, const char **url, size_t *len)
{
    float dy = y + v->scroll;
    for (int i = 0; i < v->laid; ++i) {
        ViewItem *it = &v->items[i];
        float y0 = item_top(it);
        float y1 = is_boxed(it->block) ? it->box_y + it->box_h : it->y + it->h;
        if (dy < y0 || dy > y1) continue;
        if (it->block->kind == BLOCK_TABLE) {
            for (uint32_t c = 0; c < it->block->cell_count; ++c) {
                ViewCell *vc = &it->cells[c];
                if (para_link_at(v, &vc->para, it->x + vc->x, it->y + vc->y, x, dy, url, len)) return true;
            }
            return false;
        }
        return para_link_at(v, &it->para, it->x, it->y, x, dy, url, len);
    }
    return false;
}

void view_draw_message(View *v, SDL_Renderer *r, const char *title, const char *body)
{
    const float s = v->scale;
    TTF_Font *tf = fonts_get(FONT_LIGHT, 30 * s);
    TTF_Font *bf = fonts_get(FONT_REGULAR, 15 * s);
    if (!v->msg_title) {
        v->msg_title = TTF_CreateText(v->engine, tf, title, 0);
        v->msg_body = TTF_CreateText(v->engine, bf, body, 0);
    } else {
        TTF_SetTextFont(v->msg_title, tf);
        TTF_SetTextFont(v->msg_body, bf);
        TTF_SetTextString(v->msg_title, title, 0);
        TTF_SetTextString(v->msg_body, body, 0);
    }
    TTF_SetTextColor(v->msg_title, v->theme.text.r, v->theme.text.g, v->theme.text.b, 0xFF);
    TTF_SetTextColor(v->msg_body, v->theme.muted.r, v->theme.muted.g, v->theme.muted.b, 0xFF);
    TTF_SetTextWrapWidth(v->msg_body, (int)SDL_max(v->width - 64 * s, 1.0f));

    int tw, th, bw, bh;
    TTF_GetTextSize(v->msg_title, &tw, &th);
    TTF_GetTextSize(v->msg_body, &bw, &bh);
    float y = SDL_floorf((v->height - (float)(th + bh) - 12 * s) * 0.5f);
    TTF_DrawRendererText(v->msg_title, SDL_floorf((v->width - (float)tw) * 0.5f), y);
    TTF_DrawRendererText(v->msg_body, SDL_floorf((v->width - (float)bw) * 0.5f), y + (float)th + 12 * s);
}

/* ---------------------------------------------------------- selection -- */

static int pos_cmp(SelPos a, SelPos b)
{
    if (a.frag != b.frag) return a.frag < b.frag ? -1 : 1;
    return a.off < b.off ? -1 : a.off > b.off ? 1 : 0;
}

static bool hit(View *v, float x, float y, SelPos *out)
{
    if (!v->doc || v->frag_count == 0) return false;
    float dx = x, dy = y + v->scroll;
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < v->frag_count; ++i) {
        const Frag *f = &v->frags[i];
        float vy = dy < f->ay ? f->ay - dy : dy > f->ay + f->h ? dy - (f->ay + f->h) : 0;
        float vx = dx < f->ax ? f->ax - dx : dx > f->ax + f->w ? dx - (f->ax + f->w) : 0;
        float d = vy * 1000.0f + vx;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return false;
    const Frag *f = &v->frags[best];
    out->frag = best;
    if (dx <= f->ax || f->img) out->off = (dx > f->ax + f->w * 0.5f && f->img) ? f->len : 0;
    else if (dx >= f->ax + f->w) out->off = f->len;
    else {
        size_t bytes = 0;
        int mw = 0;
        TTF_MeasureString(f->font, v->doc->strings + f->str, f->len, (int)(dx - f->ax), &mw, &bytes);
        out->off = (uint32_t)bytes;
    }
    if (dy > f->ay + f->h && best_d >= 1000.0f) out->off = f->len; /* below the line: to its end */
    return true;
}

void view_select_start(View *v, float x, float y)
{
    SelPos p;
    if (!hit(v, x, y, &p)) { v->has_sel = false; return; }
    v->sel_anchor = v->sel_focus = p;
    v->has_sel = true;
}

void view_select_extend(View *v, float x, float y)
{
    SelPos p;
    if (!v->has_sel || !hit(v, x, y, &p)) return;
    v->sel_focus = p;
}

void view_select_all(View *v)
{
    if (!v->doc) return;
    layout_until(v, v->item_count, 0);
    if (v->frag_count == 0) return;
    v->sel_anchor.frag = 0;
    v->sel_anchor.off = 0;
    v->sel_focus.frag = v->frag_count - 1;
    v->sel_focus.off = v->frags[v->frag_count - 1].len;
    v->has_sel = true;
}

void view_select_clear(View *v) { v->has_sel = false; }

bool view_has_selection(const View *v)
{
    return v->has_sel && pos_cmp(v->sel_anchor, v->sel_focus) != 0;
}

static void sel_range(const View *v, SelPos *a, SelPos *b)
{
    if (pos_cmp(v->sel_anchor, v->sel_focus) <= 0) { *a = v->sel_anchor; *b = v->sel_focus; }
    else { *a = v->sel_focus; *b = v->sel_anchor; }
}

static float measure_prefix(const Frag *f, const char *str, uint32_t off)
{
    if (off == 0) return 0;
    if (off >= f->len || f->img) return f->w;
    int w = 0, h = 0;
    TTF_GetStringSize(f->font, str, off, &w, &h);
    return (float)w;
}

void view_draw_selection(View *v, SDL_Renderer *r)
{
    if (!view_has_selection(v)) return;
    SelPos a, b;
    sel_range(v, &a, &b);
    SDL_Color c = v->theme.accent;
    c.a = 0x48;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    set_color(r, c);
    const float top = v->scroll, bottom = v->scroll + v->height;
    for (int i = a.frag; i <= b.frag && i < v->frag_count; ++i) {
        const Frag *f = &v->frags[i];
        if (f->ay + f->h < top) continue;
        if (f->ay > bottom) break;
        const char *str = v->doc->strings + f->str;
        float x0 = i == a.frag ? measure_prefix(f, str, a.off) : 0;
        float x1 = i == b.frag ? measure_prefix(f, str, b.off) : f->w;
        /* Bridge the gap (spaces) to the next selected fragment on this line. */
        if (i < b.frag && i + 1 < v->frag_count) {
            const Frag *n = &v->frags[i + 1];
            if (SDL_fabsf(n->ay - f->ay) < 1.0f && n->ax > f->ax) x1 = n->ax - f->ax;
        }
        if (x1 <= x0) continue;
        SDL_FRect rect = {f->ax + x0, f->ay - v->scroll, x1 - x0, f->h};
        SDL_RenderFillRect(r, &rect);
    }
}

char *view_selection_text(View *v)
{
    if (!view_has_selection(v)) return NULL;
    SelPos a, b;
    sel_range(v, &a, &b);
    size_t cap = 256, len = 0;
    char *out = SDL_malloc(cap);
    for (int i = a.frag; i <= b.frag && i < v->frag_count; ++i) {
        const Frag *f = &v->frags[i];
        uint32_t s = i == a.frag ? a.off : 0;
        uint32_t e = i == b.frag ? b.off : f->len;
        if (i > a.frag) {
            const Frag *p = &v->frags[i - 1];
            const char *sep = NULL;
            if (p->item != f->item) sep = "\n";
            else if (SDL_fabsf(p->ay - f->ay) >= 1.0f) {
                bool pre = v->items[f->item].block->kind == BLOCK_CODE ||
                           v->items[f->item].block->kind == BLOCK_HTML ||
                           v->items[f->item].block->kind == BLOCK_TABLE;
                sep = pre ? "\n" : " ";
            } else if (f->ax > p->ax + p->w + 0.5f) {
                sep = v->items[f->item].block->kind == BLOCK_TABLE ? "\t" : " ";
            }
            if (sep) {
                if (len + 2 > cap) { cap *= 2; out = SDL_realloc(out, cap); }
                out[len++] = sep[0];
            }
        }
        if (f->img || e <= s) continue;
        size_t n = e - s;
        if (len + n + 2 > cap) { while (len + n + 2 > cap) cap *= 2; out = SDL_realloc(out, cap); }
        SDL_memcpy(out + len, v->doc->strings + f->str + s, n);
        len += n;
    }
    out[len] = 0;
    return out;
}
