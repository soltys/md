/*
 * Outline panel and heading anchors.
 *
 * Anchors follow GitHub's rule, so links written for GitHub work in md:
 * lower case, punctuation and emoji dropped, spaces become hyphens, and
 * repeated titles get -1, -2, ... in document order.
 */
#include "outline.h"
#include "fonts.h"
#include "text.h"

#define ROW_H 26.0f
#define HEADER_H 38.0f
#define PAD_X 14.0f
#define INDENT 14.0f
#define FONT_PX 14.0f

/* ------------------------------------------------------------ slugs -- */

static Uint32 lower_cp(Uint32 c)
{
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 0x20; /* Greek */
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;               /* Cyrillic */
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

/* Symbols, punctuation and emoji, which GitHub leaves out of anchors. */
static bool dropped_cp(Uint32 c)
{
    if (c < 0x80) {
        return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == ' ' || c == '-' || c == '_');
    }
    return (c >= 0xA0 && c <= 0xBF) || c == 0xD7 || c == 0xF7 ||
           (c >= 0x2000 && c <= 0x2BFF) ||  /* punctuation, arrows, symbols */
           (c >= 0x3000 && c <= 0x303F) ||  /* CJK punctuation */
           (c >= 0xFE00 && c <= 0xFE0F) ||  /* variation selectors */
           (c >= 0x1F000 && c <= 0x1FAFF) || /* emoji */
           c >= 0xE0000;
}

static void put_utf8(char *out, size_t *n, Uint32 c)
{
    char buf[4];
    char *end = SDL_UCS4ToUTF8(c, buf);
    for (char *p = buf; p < end; ++p) out[(*n)++] = *p;
}

/* GitHub's anchor for a title (without the -1, -2 for repeats). */
static char *slugify(const char *s, size_t len)
{
    char *out = SDL_malloc(len * 2 + 1);
    if (!out) return NULL;
    size_t n = 0;
    while (len > 0) {
        Uint32 c = SDL_StepUTF8(&s, &len);
        if (c == 0) break;
        if (dropped_cp(c)) continue;
        if (c == ' ') { out[n++] = '-'; continue; }
        put_utf8(out, &n, lower_cp(c));
    }
    out[n] = 0;
    return out;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "#Za%C5%BC" -> "zaż": what a link's anchor means, for comparing. */
static char *normalize_anchor(const char *a)
{
    if (*a == '#') ++a;
    size_t len = SDL_strlen(a);
    char *dec = SDL_malloc(len + 1);
    if (!dec) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        int hi, lo;
        if (a[i] == '%' && i + 2 < len && (hi = hex(a[i + 1])) >= 0 && (lo = hex(a[i + 2])) >= 0) {
            dec[n++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            dec[n++] = a[i];
        }
    }
    dec[n] = 0;
    char *out = SDL_malloc(n * 2 + 1);
    if (out) {
        const char *p = dec;
        size_t left = n, k = 0;
        while (left > 0) {
            Uint32 c = SDL_StepUTF8(&p, &left);
            if (c == 0) break;
            put_utf8(out, &k, lower_cp(c));
        }
        out[k] = 0;
    }
    SDL_free(dec);
    return out;
}

/* ---------------------------------------------------------- entries -- */

void outline_drop_texts(Outline *o)
{
    for (int i = 0; i < o->count; ++i) {
        if (o->entries[i].text) TTF_DestroyText(o->entries[i].text);
        o->entries[i].text = NULL;
    }
    if (o->header) TTF_DestroyText(o->header);
    o->header = NULL;
    o->text_font = NULL;
}

static void free_entries(Outline *o)
{
    outline_drop_texts(o);
    for (int i = 0; i < o->count; ++i) {
        SDL_free(o->entries[i].title);
        SDL_free(o->entries[i].slug);
    }
    SDL_free(o->entries);
    o->entries = NULL;
    o->count = 0;
}

void outline_free(Outline *o)
{
    free_entries(o);
    o->doc = NULL;
}

void outline_sync(Outline *o, const Document *doc)
{
    free_entries(o);
    o->doc = doc;
    o->hover = o->current = o->shown_current = -1;
    o->min_level = 1;
    if (!doc || doc->error) return;

    int n = 0;
    for (int i = 0; i < doc->block_count; ++i) n += doc->blocks[i].kind == BLOCK_HEADING;
    if (n == 0) return;
    o->entries = SDL_calloc((size_t)n, sizeof(OutlineEntry));
    if (!o->entries) return;
    o->min_level = 6;
    for (int i = 0; i < doc->block_count; ++i) {
        const Block *b = &doc->blocks[i];
        if (b->kind != BLOCK_HEADING) continue;
        OutlineEntry *e = &o->entries[o->count];
        e->block = i;
        e->level = SDL_clamp((int)b->level, 1, 6);
        if (e->level < o->min_level) o->min_level = e->level;
        e->title = SDL_strndup(document_str(doc, b->text), b->text_len);
        if (e->title) {
            for (char *p = e->title; *p; ++p) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        char *base = slugify(e->title ? e->title : "", e->title ? SDL_strlen(e->title) : 0);
        /* Repeats: "intro", "intro-1", "intro-2", ... */
        char *slug = base;
        for (int k = 1; slug; ++k) {
            bool taken = false;
            for (int j = 0; j < o->count && !taken; ++j) taken = SDL_strcmp(o->entries[j].slug, slug) == 0;
            if (!taken) break;
            if (slug != base) SDL_free(slug);
            slug = NULL;
            SDL_asprintf(&slug, "%s-%d", base, k);
        }
        if (slug != base) SDL_free(base);
        e->slug = slug ? slug : SDL_strdup("");
        ++o->count;
    }
}

int outline_find(const Outline *o, const char *anchor)
{
    char *want = normalize_anchor(anchor);
    if (!want) return -1;
    int found = -1;
    for (int i = 0; i < o->count && found < 0; ++i) {
        if (SDL_strcmp(o->entries[i].slug, want) == 0) found = o->entries[i].block;
    }
    /* Hyphens at either end depend on what is around the text (GitHub
     * makes "-usage" of "![icon](x.png) Usage"): ignore them if nothing
     * matched exactly. */
    const char *w = want;
    while (*w == '-') ++w;
    size_t wn = SDL_strlen(w);
    while (wn > 0 && w[wn - 1] == '-') --wn;
    for (int i = 0; i < o->count && found < 0 && wn > 0; ++i) {
        const char *e = o->entries[i].slug;
        while (*e == '-') ++e;
        size_t en = SDL_strlen(e);
        while (en > 0 && e[en - 1] == '-') --en;
        if (en == wn && SDL_memcmp(e, w, wn) == 0) found = o->entries[i].block;
    }
    SDL_free(want);
    return found;
}

int outline_entry_at_line(const Outline *o, int line)
{
    if (!o->doc) return -1;
    int best = -1;
    for (int i = 0; i < o->count; ++i) {
        if ((int)o->doc->blocks[o->entries[i].block].src_line <= line) best = i;
        else break;
    }
    return best;
}

/* ------------------------------------------------------------ panel -- */

float outline_width(const Outline *o, float area_w, float scale)
{
    if (!o->visible) return 0;
    return SDL_floorf(SDL_min(260.0f * scale, area_w * 0.4f));
}

static float rows_top(float scale) { return SDL_floorf(HEADER_H * scale); }

static float max_scroll(const Outline *o, const SDL_FRect *rect, float scale)
{
    float content = (float)o->count * SDL_floorf(ROW_H * scale) + SDL_floorf(8 * scale);
    return SDL_max(0.0f, content - (rect->h - rows_top(scale)));
}

void outline_scroll_by(Outline *o, const SDL_FRect *rect, float dy, float scale)
{
    o->scroll = SDL_clamp(o->scroll + dy, 0.0f, max_scroll(o, rect, scale));
}

int outline_hit(const Outline *o, const SDL_FRect *rect, float x, float y, float scale)
{
    if (!o->visible || x < rect->x || x >= rect->x + rect->w || y < rect->y || y >= rect->y + rect->h) return -1;
    float ry = y - rect->y - rows_top(scale);
    if (ry < 0) return -1;
    int i = (int)((ry + o->scroll) / SDL_floorf(ROW_H * scale));
    return i >= 0 && i < o->count ? i : -1;
}

bool outline_set_hover(Outline *o, int entry)
{
    if (entry == o->hover) return false;
    o->hover = entry;
    return true;
}

/* A title cut to max_w pixels, with "…" when it does not fit. */
static TTF_Text *make_title(TTF_TextEngine *engine, TTF_Font *font, const char *s, float max_w)
{
    size_t len = SDL_strlen(s);
    int w = 0;
    size_t fit = 0;
    TTF_MeasureString(font, s, len, max_w > 0 ? (int)max_w : 0, &w, &fit);
    if (fit >= len) return TTF_CreateText(engine, font, s, len);
    int ell_w = 0;
    TTF_GetStringSize(font, "\xE2\x80\xA6", 0, &ell_w, NULL);
    TTF_MeasureString(font, s, len, SDL_max(0, (int)max_w - ell_w), &w, &fit);
    while (fit > 0 && ((unsigned char)s[fit] & 0xC0) == 0x80) --fit; /* whole characters */
    while (fit > 0 && s[fit - 1] == ' ') --fit;
    char *cut = NULL;
    SDL_asprintf(&cut, "%.*s\xE2\x80\xA6", (int)fit, s);
    TTF_Text *t = cut ? TTF_CreateText(engine, font, cut, 0) : NULL;
    SDL_free(cut);
    return t;
}

void outline_draw(Outline *o, SDL_Renderer *r, TTF_TextEngine *engine, const SDL_FRect *rect,
                  const Theme *th, float scale)
{
    if (!o->visible || rect->w <= 0 || rect->h <= 0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, th->code_bg.r, th->code_bg.g, th->code_bg.b, 0xFF);
    SDL_RenderFillRect(r, rect);

    TTF_Font *font = fonts_get(FONT_REGULAR, FONT_PX * scale);
    TTF_Font *head_font = fonts_get(FONT_MEDIUM, 12.0f * scale);
    if (!font || !head_font) return;
    const float pad = SDL_floorf(PAD_X * scale);
    const float text_w = rect->w - 2 * pad;
    if (font != o->text_font || text_w != o->text_w) {
        outline_drop_texts(o);
        o->text_font = font;
        o->text_w = text_w;
        bool emoji = false;
        for (int i = 0; i < o->count && !emoji; ++i) {
            for (const unsigned char *p = (const unsigned char *)o->entries[i].title; p && *p; ++p) {
                if (*p >= 0xF0) { emoji = true; break; }
            }
        }
        if (emoji) fonts_get_emoji(FONT_PX * scale); /* joins the fallbacks */
    }

    SDL_Rect clip = {(int)rect->x, (int)rect->y, (int)rect->w, (int)rect->h};
    SDL_SetRenderClipRect(r, &clip);

    if (!o->header) o->header = TTF_CreateText(engine, head_font, o->count ? "OUTLINE" : "NO HEADINGS", 0);
    if (o->header) {
        int hw = 0, hh = 0;
        TTF_GetTextSize(o->header, &hw, &hh);
        TTF_SetTextColor(o->header, th->muted.r, th->muted.g, th->muted.b, 0xFF);
        md_text_draw(o->header, rect->x + pad, rect->y + SDL_floorf((HEADER_H * scale - (float)hh) * 0.5f));
    }

    const float row = SDL_floorf(ROW_H * scale);
    const float top = rect->y + rows_top(scale);
    /* Keep the section on screen in view when it changes by scrolling the
     * document (not while the reader scrolls the panel itself). */
    if (o->current != o->shown_current) {
        o->shown_current = o->current;
        if (o->current >= 0) {
            float y = (float)o->current * row, view_h = rect->h - rows_top(scale);
            if (y < o->scroll) o->scroll = y;
            else if (y + row > o->scroll + view_h) o->scroll = y + row - view_h;
        }
    }
    o->scroll = SDL_clamp(o->scroll, 0.0f, max_scroll(o, rect, scale));

    SDL_Rect rows_clip = {clip.x, (int)top, clip.w, (int)(rect->y + rect->h - top)};
    SDL_SetRenderClipRect(r, &rows_clip);
    int first = (int)(o->scroll / row);
    for (int i = SDL_max(0, first); i < o->count; ++i) {
        float y = top + (float)i * row - o->scroll;
        if (y >= rect->y + rect->h) break;
        OutlineEntry *e = &o->entries[i];
        SDL_FRect bg = {rect->x, y, rect->w, row};
        if (i == o->hover) {
            SDL_SetRenderDrawColor(r, th->text.r, th->text.g, th->text.b, 0x17);
            SDL_RenderFillRect(r, &bg);
        }
        if (i == o->current) {
            SDL_FRect bar = {rect->x, y + SDL_floorf(4 * scale), SDL_max(2.0f, SDL_floorf(3 * scale)),
                             row - SDL_floorf(8 * scale)};
            SDL_SetRenderDrawColor(r, th->accent.r, th->accent.g, th->accent.b, 0xFF);
            SDL_RenderFillRect(r, &bar);
        }
        const float indent = (float)(e->level - o->min_level) * SDL_floorf(INDENT * scale);
        if (!e->text) e->text = make_title(engine, font, e->title ? e->title : "", text_w - indent);
        if (!e->text) continue;
        int tw = 0, tht = 0;
        TTF_GetTextSize(e->text, &tw, &tht);
        SDL_Color c = i == o->current ? th->accent : e->level <= o->min_level ? th->heading : th->text;
        TTF_SetTextColor(e->text, c.r, c.g, c.b, 0xFF);
        md_text_draw(e->text, rect->x + pad + indent, y + SDL_floorf((row - (float)tht) * 0.5f));
    }
    SDL_SetRenderClipRect(r, NULL);
}
