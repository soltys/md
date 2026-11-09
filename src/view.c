#include "view.h"
#include "fonts.h"

#define BODY_PX       16.0f
#define MAX_WIDTH     920.0f
#define MARGIN        32.0f
#define LIST_INDENT   26.0f
#define QUOTE_INDENT  18.0f

static const float heading_scale[7] = {1.0f, 2.0f, 1.5f, 1.25f, 1.0f, 0.875f, 0.85f};

static void set_color(SDL_Renderer *r, SDL_Color c)
{
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

static void text_color(TTF_Text *t, SDL_Color c)
{
    if (t) TTF_SetTextColor(t, c.r, c.g, c.b, c.a);
}

static FontFace block_face(const Block *b)
{
    switch (b->kind) {
    case BLOCK_HEADING: return FONT_BOLD;
    case BLOCK_CODE:
    case BLOCK_TABLE:
    case BLOCK_HTML:    return FONT_MONO;
    default:            return FONT_REGULAR;
    }
}

static float block_px(const View *v, const Block *b)
{
    float base = BODY_PX * v->scale;
    switch (b->kind) {
    case BLOCK_HEADING: return base * heading_scale[b->level <= 6 ? b->level : 6];
    case BLOCK_CODE:
    case BLOCK_TABLE:
    case BLOCK_HTML:    return base * 0.875f;
    default:            return base;
    }
}

static void free_items(View *v)
{
    for (int i = 0; i < v->item_count; ++i) {
        if (v->items[i].text) TTF_DestroyText(v->items[i].text);
        if (v->items[i].marker) TTF_DestroyText(v->items[i].marker);
        if (v->items[i].lang) TTF_DestroyText(v->items[i].lang);
    }
    SDL_free(v->items);
    v->items = NULL;
    v->item_count = 0;
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
    if (!doc || doc->error) return;

    v->items = SDL_calloc((size_t)(doc->block_count ? doc->block_count : 1), sizeof(ViewItem));
    v->item_count = doc->block_count;
    const Theme *th = &v->theme;
    for (int i = 0; i < doc->block_count; ++i) {
        const Block *b = &doc->blocks[i];
        ViewItem *it = &v->items[i];
        it->block = b;
        TTF_Font *font = fonts_get(block_face(b), block_px(v, b));
        if (b->kind != BLOCK_RULE) {
            it->text = TTF_CreateText(v->engine, font, document_str(doc, b->text), b->text_len);
            text_color(it->text, b->kind == BLOCK_HEADING ? th->heading
                               : b->kind == BLOCK_CODE ? th->code_text
                               : b->quote_depth ? th->muted : th->text);
        }
        if (b->marker_len) {
            TTF_Font *mf = fonts_get(FONT_REGULAR, BODY_PX * v->scale);
            it->marker = TTF_CreateText(v->engine, mf, document_str(doc, b->marker), b->marker_len);
            text_color(it->marker, th->muted);
        }
        if (b->lang_len) {
            TTF_Font *lf = fonts_get(FONT_MEDIUM, BODY_PX * 0.75f * v->scale);
            it->lang = TTF_CreateText(v->engine, lf, document_str(doc, b->lang), b->lang_len);
            text_color(it->lang, th->muted);
        }
    }
    /* Force a relayout with the current geometry. */
    float w = v->width, h = v->height;
    v->width = 0;
    view_layout(v, w, h, v->scale);
    if (keep_scroll) view_scroll_to(v, old_scroll);
}

void view_scroll_to_line(View *v, int line)
{
    if (!v->doc || v->item_count == 0) return;
    int best = 0;
    for (int i = 0; i < v->item_count; ++i) {
        if ((int)v->items[i].block->src_line <= line) best = i;
        else break;
    }
    const ViewItem *it = &v->items[best];
    const Block *b = it->block;
    float y = (b->kind == BLOCK_CODE || b->kind == BLOCK_TABLE || b->kind == BLOCK_HTML) ? it->box_y : it->y;
    view_scroll_to(v, y - MARGIN * v->scale);
}

static void refresh_fonts(View *v)
{
    for (int i = 0; i < v->item_count; ++i) {
        ViewItem *it = &v->items[i];
        if (it->text) TTF_SetTextFont(it->text, fonts_get(block_face(it->block), block_px(v, it->block)));
        if (it->marker) TTF_SetTextFont(it->marker, fonts_get(FONT_REGULAR, BODY_PX * v->scale));
        if (it->lang) TTF_SetTextFont(it->lang, fonts_get(FONT_MEDIUM, BODY_PX * 0.75f * v->scale));
    }
}

void view_layout(View *v, float width, float height, float scale)
{
    bool scale_changed = scale != v->scale;
    bool width_changed = width != v->width;
    v->height = height;
    v->width = width;
    v->scale = scale;
    if (scale_changed) refresh_fonts(v);
    if (!scale_changed && !width_changed) {
        view_scroll_by(v, 0);
        return;
    }

    const float s = scale;
    float content_w = SDL_min(MAX_WIDTH * s, width - 2 * MARGIN * s);
    if (content_w < 100 * s) content_w = SDL_max(width - 16 * s, 50.0f);
    const float x0 = SDL_floorf((width - content_w) * 0.5f);
    const float code_pad = 14 * s;

    float y = MARGIN * s;
    for (int i = 0; i < v->item_count; ++i) {
        ViewItem *it = &v->items[i];
        const Block *b = it->block;
        bool in_list = b->list_depth > 0;
        float indent = b->list_depth * LIST_INDENT * s + b->quote_depth * QUOTE_INDENT * s;
        float x = x0 + indent;
        float w = content_w - indent;

        /* Spacing before the block. */
        if (i > 0) {
            if (b->kind == BLOCK_HEADING) y += 10 * s;
        }

        if (b->kind == BLOCK_CODE || b->kind == BLOCK_TABLE || b->kind == BLOCK_HTML) {
            x += code_pad;
            w -= 2 * code_pad;
        }

        int tw = 0, th = 0;
        if (it->text) {
            TTF_SetTextWrapWidth(it->text, (int)SDL_max(w, 1.0f));
            TTF_GetTextSize(it->text, &tw, &th);
        }
        if (th == 0 && it->marker) {
            int mw;
            TTF_GetTextSize(it->marker, &mw, &th);
        }

        it->x = x;
        it->w = w;
        it->h = (float)th;

        switch (b->kind) {
        case BLOCK_RULE:
            y += 8 * s;
            it->y = y;
            it->h = SDL_max(2 * s, 1.0f);
            y += it->h + 24 * s;
            break;
        case BLOCK_CODE:
        case BLOCK_TABLE:
        case BLOCK_HTML: {
            float label_h = 0;
            if (it->lang) {
                int lw, lh;
                TTF_GetTextSize(it->lang, &lw, &lh);
                label_h = (float)lh + 4 * s;
            }
            it->box_y = y;
            it->y = y + code_pad + label_h;
            it->box_h = it->h + 2 * code_pad + label_h;
            y += it->box_h + 16 * s;
            break;
        }
        case BLOCK_HEADING:
            it->y = y;
            y += it->h + (b->level <= 2 ? 24 * s : 12 * s);
            break;
        default:
            it->y = y;
            y += it->h + (in_list ? 6 * s : 16 * s);
            break;
        }
    }
    v->content_height = y + MARGIN * s;
    view_scroll_by(v, 0);
}

float view_line_height(const View *v)
{
    return BODY_PX * 1.5f * v->scale;
}

void view_scroll_to(View *v, float y)
{
    float max_scroll = SDL_max(0.0f, v->content_height - v->height);
    v->scroll = SDL_clamp(y, 0.0f, max_scroll);
}

void view_scroll_by(View *v, float dy)
{
    view_scroll_to(v, v->scroll + dy);
}

static void draw_checkbox(SDL_Renderer *r, const Theme *th, float x, float y, float size,
                          bool checked, float s)
{
    SDL_FRect box = {x, y, size, size};
    if (checked) {
        set_color(r, th->accent);
        SDL_RenderFillRect(r, &box);
        /* simple check mark */
        set_color(r, th->bg);
        for (float t = 0; t < 1.5f * s; t += 0.5f) {
            SDL_RenderLine(r, x + size * 0.22f, y + size * 0.52f + t,
                           x + size * 0.42f, y + size * 0.72f + t);
            SDL_RenderLine(r, x + size * 0.42f, y + size * 0.72f + t,
                           x + size * 0.78f, y + size * 0.30f + t);
        }
    } else {
        set_color(r, th->muted);
        SDL_RenderRect(r, &box);
    }
}

void view_draw(View *v, SDL_Renderer *r)
{
    const Theme *th = &v->theme;
    const float s = v->scale;
    const float top = v->scroll;
    const float bottom = v->scroll + v->height;

    for (int i = 0; i < v->item_count; ++i) {
        ViewItem *it = &v->items[i];
        const Block *b = it->block;
        float y0 = (b->kind == BLOCK_CODE || b->kind == BLOCK_TABLE || b->kind == BLOCK_HTML)
                       ? it->box_y : it->y;
        float y1 = (b->kind == BLOCK_CODE || b->kind == BLOCK_TABLE || b->kind == BLOCK_HTML)
                       ? it->box_y + it->box_h : it->y + it->h;
        if (y1 < top) continue;
        if (y0 > bottom) break; /* items are laid out top to bottom */

        float dy = -v->scroll;

        /* Block quote bars. */
        for (int q = 0; q < b->quote_depth; ++q) {
            float qx = it->x - (b->quote_depth - q) * QUOTE_INDENT * s
                       - (b->kind == BLOCK_CODE ? 14 * s : 0);
            SDL_FRect bar = {qx, y0 + dy - 4 * s, 4 * s, (y1 - y0) + 8 * s + 12 * s};
            set_color(r, th->quote_bar);
            SDL_RenderFillRect(r, &bar);
        }

        switch (b->kind) {
        case BLOCK_RULE: {
            SDL_FRect line = {it->x, it->y + dy, it->w, it->h};
            set_color(r, th->rule);
            SDL_RenderFillRect(r, &line);
            break;
        }
        case BLOCK_CODE:
        case BLOCK_TABLE:
        case BLOCK_HTML: {
            float pad = 14 * s;
            SDL_FRect box = {it->x - pad, it->box_y + dy, it->w + 2 * pad, it->box_h};
            set_color(r, th->code_bg);
            SDL_RenderFillRect(r, &box);
            if (it->lang) TTF_DrawRendererText(it->lang, it->x, it->box_y + dy + pad * 0.6f);
            break;
        }
        case BLOCK_HEADING:
            if (b->level <= 2) {
                SDL_FRect line = {it->x, it->y + it->h + dy + 8 * s, it->w, SDL_max(1.0f, s)};
                set_color(r, th->rule);
                SDL_RenderFillRect(r, &line);
            }
            break;
        default:
            break;
        }

        if (it->marker && b->task == TASK_NONE) { /* task items show a checkbox instead */
            int mw, mh;
            TTF_GetTextSize(it->marker, &mw, &mh);
            float mx = it->x - (float)mw - 8 * s;
            TTF_DrawRendererText(it->marker, mx, it->y + dy);
        }

        float tx = it->x;
        if (b->task != TASK_NONE) {
            float size = 14 * s;
            draw_checkbox(r, th, tx, it->y + dy + 3 * s, size, b->task == TASK_CHECKED, s);
            tx += size + 8 * s;
        }

        if (it->text) TTF_DrawRendererText(it->text, tx, it->y + dy);
    }
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
    text_color(v->msg_title, v->theme.text);
    text_color(v->msg_body, v->theme.muted);
    TTF_SetTextWrapWidth(v->msg_body, (int)SDL_max(v->width - 64 * s, 1.0f));

    int tw, th, bw, bh;
    TTF_GetTextSize(v->msg_title, &tw, &th);
    TTF_GetTextSize(v->msg_body, &bw, &bh);
    float y = SDL_floorf((v->height - (float)(th + bh) - 12 * s) * 0.5f);
    TTF_DrawRendererText(v->msg_title, SDL_floorf((v->width - (float)tw) * 0.5f), y);
    TTF_DrawRendererText(v->msg_body, SDL_floorf((v->width - (float)bw) * 0.5f), y + (float)th + 12 * s);
}
