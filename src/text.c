/* md's text engine for SDL_ttf: grayscale or subpixel (LCD) glyphs in a
 * texture atlas. Based on SDL_ttf's SDL_renderer_textengine.c (zlib
 * licence, Copyright (C) 2001-2025 Sam Lantinga); altered for md: a simple
 * shelf-packed atlas that is never evicted, glyphs looked up at draw time,
 * and subpixel glyphs.
 *
 * Subpixel text: FreeType renders each glyph at three times the horizontal
 * resolution, one coverage value per colour channel. The atlas keeps those
 * as the texel's R, G and B. A glyph is then drawn twice:
 *   1. dst = dst * (1 - coverage)          (custom blend, per channel)
 *   2. dst = dst + text colour * coverage  (additive)
 * which is the usual "coverage per channel" blend, done without dual-source
 * blending (SDL's renderers have none). Both passes keep the destination
 * alpha. Renderers without custom blend modes (software) get grayscale. */
#include "text.h"

#include <SDL3_ttf/SDL_textengine.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H

#ifdef _WIN32
#include <windows.h>
#endif

#define ATLAS_SIZE 1024

/* ------------------------------------------------------ font registry -- */

typedef struct FontSource {
    TTF_Font *font;
    const void *data;
    size_t size;
    float px;
    TTF_HintingFlags hinting;
    FT_Face face; /* opened on first subpixel glyph */
    bool failed;
} FontSource;

#define MAX_SOURCES 1024
static FontSource sources[MAX_SOURCES];
static int source_count;
static SDL_Mutex *source_lock; /* fonts can be opened on the font thread */
static SDL_InitState source_init;

void md_text_register_font(TTF_Font *font, const void *data, size_t size, float px,
                           TTF_HintingFlags hinting)
{
    if (!font || !data) return;
    if (SDL_ShouldInit(&source_init)) {
        source_lock = SDL_CreateMutex();
        SDL_SetInitialized(&source_init, true);
    }
    SDL_LockMutex(source_lock);
    if (source_count < MAX_SOURCES) {
        sources[source_count++] = (FontSource){font, data, size, px, hinting, NULL, false};
    }
    SDL_UnlockMutex(source_lock);
}

static FontSource *find_source(TTF_Font *font)
{
    FontSource *found = NULL;
    SDL_LockMutex(source_lock);
    for (int i = source_count; i--;) {
        if (sources[i].font == font) { found = &sources[i]; break; }
    }
    SDL_UnlockMutex(source_lock);
    return found;
}

/* ------------------------------------------------------------- engine -- */

typedef enum GlyphKind { GLYPH_ALPHA, GLYPH_COLOR, GLYPH_LCD } GlyphKind;

typedef struct Atlas {
    SDL_Texture *texture;
    int x, y, row_h; /* shelf packer */
    struct Atlas *next;
} Atlas;

typedef struct Glyph {
    TTF_Font *font;
    Uint32 index;
    bool lcd; /* looked up as a subpixel glyph */
    GlyphKind kind;
    Atlas *atlas;
    SDL_Rect rect; /* in the atlas */
    int dx, dy;    /* subpixel image offset from the grayscale one */
} Glyph;

typedef struct Engine {
    SDL_Renderer *renderer;
    bool lcd_ok;   /* renderer has custom blending and subpixel is wanted */
    bool bgr;      /* BGR panel */
    bool force_gray;
    SDL_BlendMode clear_mode, add_mode;
    FT_Library ft;
    Atlas *atlas;
    Glyph **table; /* open addressing, (font, index, lcd) */
    int table_cap, table_count;
    /* scratch for building draws */
    Glyph **resolved;
    float *xy, *uv;
    int *indices;
    int scratch_cap;
} Engine;

static Uint32 hash_key(TTF_Font *font, Uint32 index, bool lcd)
{
    Uint64 h = (Uint64)(uintptr_t)font * 0x9E3779B97F4A7C15ull;
    h ^= (Uint64)index * 0xC2B2AE3D27D4EB4Full + (lcd ? 0x165667B19E3779F9ull : 0);
    h ^= h >> 29;
    return (Uint32)h;
}

static Glyph *table_find(Engine *e, TTF_Font *font, Uint32 index, bool lcd)
{
    if (!e->table_cap) return NULL;
    Uint32 mask = (Uint32)e->table_cap - 1;
    for (Uint32 i = hash_key(font, index, lcd) & mask;; i = (i + 1) & mask) {
        Glyph *g = e->table[i];
        if (!g) return NULL;
        if (g->font == font && g->index == index && g->lcd == lcd) return g;
    }
}

static bool table_insert(Engine *e, Glyph *g)
{
    if ((e->table_count + 1) * 2 > e->table_cap) {
        int cap = e->table_cap ? e->table_cap * 2 : 1024;
        Glyph **t = (Glyph **)SDL_calloc((size_t)cap, sizeof(*t));
        if (!t) return false;
        for (int i = 0; i < e->table_cap; ++i) {
            Glyph *o = e->table[i];
            if (!o) continue;
            Uint32 mask = (Uint32)cap - 1, j = hash_key(o->font, o->index, o->lcd) & mask;
            while (t[j]) j = (j + 1) & mask;
            t[j] = o;
        }
        SDL_free(e->table);
        e->table = t;
        e->table_cap = cap;
    }
    Uint32 mask = (Uint32)e->table_cap - 1, j = hash_key(g->font, g->index, g->lcd) & mask;
    while (e->table[j]) j = (j + 1) & mask;
    e->table[j] = g;
    e->table_count++;
    return true;
}

static Atlas *new_atlas(Engine *e)
{
    Atlas *a = (Atlas *)SDL_calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->texture = SDL_CreateTexture(e->renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STATIC, ATLAS_SIZE, ATLAS_SIZE);
    if (!a->texture) { SDL_free(a); return NULL; }
    SDL_SetTextureScaleMode(a->texture, SDL_SCALEMODE_NEAREST);
    return a;
}

/* Finds room for w x h (plus a 1 px gap) and uploads ARGB8888 pixels. */
static bool place(Engine *e, Glyph *g, int w, int h, const void *pixels, int pitch)
{
    g->rect = (SDL_Rect){0, 0, w, h};
    if (w <= 0 || h <= 0) { g->rect.w = g->rect.h = 0; return true; }
    if (w + 1 > ATLAS_SIZE || h + 1 > ATLAS_SIZE) return SDL_SetError("glyph %dx%d too large", w, h);
    if (!e->atlas && !(e->atlas = new_atlas(e))) return false;
    Atlas *a = e->atlas;
    for (;;) {
        if (a->x + w + 1 > ATLAS_SIZE) { a->x = 0; a->y += a->row_h; a->row_h = 0; }
        if (a->y + h + 1 <= ATLAS_SIZE) break;
        if (!a->next && !(a->next = new_atlas(e))) return false;
        a = a->next;
    }
    g->atlas = a;
    g->rect.x = a->x;
    g->rect.y = a->y;
    a->x += w + 1;
    if (h + 1 > a->row_h) a->row_h = h + 1;
    return SDL_UpdateTexture(a->texture, &g->rect, pixels, pitch);
}

static FT_Int32 load_flags(TTF_HintingFlags hinting)
{
    /* The same flags SDL_ttf loads with, so the outline (and the grayscale
     * bitmap position SDL_ttf laid the text out with) is the same. */
    switch (hinting) {
    case TTF_HINTING_LIGHT:
    case TTF_HINTING_LIGHT_SUBPIXEL: return FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
    case TTF_HINTING_MONO:           return FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO;
    case TTF_HINTING_NONE:           return FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING;
    default:                         return FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;
    }
}

/* Rasterises a subpixel glyph into g; false when the font cannot do it. */
static bool make_lcd_glyph(Engine *e, Glyph *g)
{
    FontSource *src = find_source(g->font);
    if (!src || src->failed || !e->ft) return false;
    if (!src->face) {
        if (FT_New_Memory_Face(e->ft, (const FT_Byte *)src->data, (FT_Long)src->size, 0, &src->face) ||
            !FT_IS_SCALABLE(src->face) || FT_HAS_COLOR(src->face) ||
            FT_Set_Char_Size(src->face, 0, (FT_F26Dot6)SDL_roundf(src->px * 64), 72, 72)) {
            if (src->face) FT_Done_Face(src->face);
            src->face = NULL;
            src->failed = true;
            return false;
        }
    }
    FT_Face face = src->face;
    if (FT_Load_Glyph(face, g->index, load_flags(src->hinting))) return false;
    FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE) return false;
    /* FreeType presets the grayscale bitmap box at load time: that is where
     * SDL_ttf put this glyph. */
    const int gray_left = slot->bitmap_left, gray_top = slot->bitmap_top;
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_LCD)) return false;

    const FT_Bitmap *bm = &slot->bitmap;
    const int w = (int)bm->width / 3, h = (int)bm->rows;
    g->kind = GLYPH_LCD;
    g->dx = slot->bitmap_left - gray_left;
    g->dy = gray_top - slot->bitmap_top;
    if (w <= 0 || h <= 0 || !bm->buffer) return place(e, g, 0, 0, NULL, 0);

    Uint32 *px = (Uint32 *)SDL_malloc((size_t)w * (size_t)h * 4);
    if (!px) return false;
    for (int y = 0; y < h; ++y) {
        const Uint8 *s = bm->buffer + (ptrdiff_t)y * bm->pitch;
        Uint32 *d = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x, s += 3) {
            Uint32 r = s[0], gr = s[1], b = s[2];
            if (e->bgr) { Uint32 t = r; r = b; b = t; }
            Uint32 a = SDL_max(r, SDL_max(gr, b));
            d[x] = (a << 24) | (r << 16) | (gr << 8) | b;
        }
    }
    bool ok = place(e, g, w, h, px, w * 4);
    SDL_free(px);
    return ok;
}

static Glyph *get_glyph(Engine *e, TTF_Font *font, Uint32 index, bool lcd)
{
    Glyph *g = table_find(e, font, index, lcd);
    if (g) return g;
    g = (Glyph *)SDL_calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->font = font;
    g->index = index;
    g->lcd = lcd;
    bool ok = false;
    if (lcd) {
        /* Colour glyphs (emoji) stay as SDL_ttf makes them. */
        ok = make_lcd_glyph(e, g);
        if (!ok) {
            Glyph *plain = get_glyph(e, font, index, false);
            if (plain) { *g = *plain; g->lcd = true; ok = true; }
        }
    } else {
        TTF_ImageType type = TTF_IMAGE_INVALID;
        SDL_Surface *s = TTF_GetGlyphImageForIndex(font, index, &type);
        if (s) {
            g->kind = type == TTF_IMAGE_ALPHA ? GLYPH_ALPHA : GLYPH_COLOR;
            if (s->format != SDL_PIXELFORMAT_ARGB8888) {
                SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
                SDL_DestroySurface(s);
                s = c;
            }
            ok = s && place(e, g, s->w, s->h, s->pixels, s->pitch);
            SDL_DestroySurface(s);
        }
    }
    if (!ok || !table_insert(e, g)) { SDL_free(g); return NULL; }
    return g;
}

static bool SDLCALL create_text(void *userdata, TTF_Text *text)
{
    (void)userdata;
    /* Glyphs are looked up when drawing; nothing per text to keep. */
    static char marker;
    text->internal->engine_text = &marker;
    return true;
}

static void SDLCALL destroy_text(void *userdata, TTF_Text *text)
{
    (void)userdata;
    (void)text;
}

static bool want_subpixel(bool *bgr)
{
    *bgr = false;
    const char *env = SDL_getenv("MD_FONT_AA");
    if (env) {
        if (SDL_strcasecmp(env, "grayscale") == 0 || SDL_strcasecmp(env, "gray") == 0) return false;
        if (SDL_strcasecmp(env, "bgr") == 0) { *bgr = true; return true; }
        if (SDL_strcasecmp(env, "subpixel") == 0 || SDL_strcasecmp(env, "rgb") == 0) return true;
    }
#ifdef _WIN32
    /* Follow the ClearType setting: off, or standard smoothing, means
     * grayscale; the panel's subpixel order comes from ClearType Tuner. */
    BOOL smoothing = TRUE;
    UINT type = 0, order = 1;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smoothing, 0) && !smoothing) return false;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &type, 0) && type != FE_FONTSMOOTHINGCLEARTYPE) return false;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0, &order, 0))
        *bgr = order == FE_FONTSMOOTHINGORIENTATIONBGR;
    return true;
#else
    return env != NULL; /* Linux builds: grayscale unless asked */
#endif
}

TTF_TextEngine *md_text_engine_create(SDL_Renderer *renderer)
{
    if (!renderer) return NULL;
    TTF_TextEngine *engine = (TTF_TextEngine *)SDL_calloc(1, sizeof(*engine));
    Engine *e = (Engine *)SDL_calloc(1, sizeof(*e));
    if (!engine || !e) { SDL_free(engine); SDL_free(e); return NULL; }
    SDL_INIT_INTERFACE(engine);
    engine->CreateText = create_text;
    engine->DestroyText = destroy_text;
    engine->userdata = e;
    e->renderer = renderer;
    e->clear_mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    e->add_mode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
    if (want_subpixel(&e->bgr) && !FT_Init_FreeType(&e->ft)) {
        FT_Library_SetLcdFilter(e->ft, FT_LCD_FILTER_DEFAULT); /* no-op on Harmony builds */
        /* The atlas doubles as the probe: renderers without custom blend
         * modes refuse them. */
        e->atlas = new_atlas(e);
        e->lcd_ok = e->atlas && SDL_SetTextureBlendMode(e->atlas->texture, e->clear_mode) &&
                    SDL_SetTextureBlendMode(e->atlas->texture, e->add_mode);
        if (e->atlas) SDL_SetTextureBlendMode(e->atlas->texture, SDL_BLENDMODE_BLEND);
    }
    return engine;
}

void md_text_engine_destroy(TTF_TextEngine *engine)
{
    if (!engine || engine->CreateText != create_text) return;
    Engine *e = (Engine *)engine->userdata;
    for (int i = 0; i < e->table_cap; ++i) SDL_free(e->table[i]);
    SDL_free(e->table);
    for (Atlas *a = e->atlas, *n; a; a = n) {
        n = a->next;
        SDL_DestroyTexture(a->texture);
        SDL_free(a);
    }
    SDL_LockMutex(source_lock);
    for (int i = 0; i < source_count; ++i) {
        if (sources[i].face) FT_Done_Face(sources[i].face);
        sources[i].face = NULL;
    }
    SDL_UnlockMutex(source_lock);
    if (e->ft) FT_Done_FreeType(e->ft);
    SDL_free(e->resolved);
    SDL_free(e->xy);
    SDL_free(e->uv);
    SDL_free(e->indices);
    SDL_free(e);
    engine->CreateText = NULL;
    SDL_free(engine);
}

bool md_text_subpixel(TTF_TextEngine *engine)
{
    if (!engine || engine->CreateText != create_text) return false;
    const Engine *e = (const Engine *)engine->userdata;
    return e->lcd_ok && !e->force_gray;
}

void md_text_force_grayscale(TTF_TextEngine *engine, bool gray)
{
    if (!engine || engine->CreateText != create_text) return;
    ((Engine *)engine->userdata)->force_gray = gray;
}

static bool grow_scratch(Engine *e, int n)
{
    if (n <= e->scratch_cap) return true;
    int cap = SDL_max(n, e->scratch_cap * 2);
    Glyph **r = (Glyph **)SDL_realloc(e->resolved, (size_t)cap * sizeof(*r));
    if (r) e->resolved = r;
    float *xy = (float *)SDL_realloc(e->xy, (size_t)cap * 8 * sizeof(float));
    if (xy) e->xy = xy;
    float *uv = (float *)SDL_realloc(e->uv, (size_t)cap * 8 * sizeof(float));
    if (uv) e->uv = uv;
    int *ix = (int *)SDL_realloc(e->indices, (size_t)cap * 6 * sizeof(int));
    if (ix) e->indices = ix;
    if (!r || !xy || !uv || !ix) return false;
    e->scratch_cap = cap;
    return true;
}

static void draw_batch(Engine *e, SDL_Texture *tex, int quads, GlyphKind kind, SDL_FColor c)
{
    if (!quads) return;
    if (kind == GLYPH_LCD) {
        const SDL_FColor cover = {c.a, c.a, c.a, 1.0f};
        const SDL_FColor ink = {c.r * c.a, c.g * c.a, c.b * c.a, 1.0f};
        SDL_SetTextureBlendMode(tex, e->clear_mode);
        SDL_RenderGeometryRaw(e->renderer, tex, e->xy, 2 * sizeof(float), &cover, 0,
                              e->uv, 2 * sizeof(float), quads * 4, e->indices, quads * 6, sizeof(int));
        SDL_SetTextureBlendMode(tex, e->add_mode);
        SDL_RenderGeometryRaw(e->renderer, tex, e->xy, 2 * sizeof(float), &ink, 0,
                              e->uv, 2 * sizeof(float), quads * 4, e->indices, quads * 6, sizeof(int));
    } else {
        SDL_FColor col = c;
        if (kind == GLYPH_COLOR) col.r = col.g = col.b = 1.0f; /* keep the image's colours */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometryRaw(e->renderer, tex, e->xy, 2 * sizeof(float), &col, 0,
                              e->uv, 2 * sizeof(float), quads * 4, e->indices, quads * 6, sizeof(int));
    }
}

bool md_text_draw(TTF_Text *text, float x, float y)
{
    if (!text || !text->internal || text->internal->engine->CreateText != create_text) {
        return SDL_InvalidParamError("text");
    }
    if (!TTF_UpdateText(text)) return false;
    Engine *e = (Engine *)text->internal->engine->userdata;
    const TTF_DrawOperation *ops = text->internal->ops;
    const int n = text->internal->num_ops;
    const SDL_FColor color = text->internal->color;
    if (n <= 0) return true;
    if (!grow_scratch(e, n)) return false;
    const bool lcd = e->lcd_ok && !e->force_gray;

    int pending = 0;
    for (int i = 0; i < n; ++i) {
        const TTF_DrawOperation *op = &ops[i];
        if (op->cmd == TTF_DRAW_COMMAND_FILL) {
            SDL_FRect r = {x + op->fill.rect.x, y + op->fill.rect.y,
                           (float)op->fill.rect.w, (float)op->fill.rect.h};
            float r0, g0, b0, a0;
            SDL_GetRenderDrawColorFloat(e->renderer, &r0, &g0, &b0, &a0);
            SDL_SetRenderDrawColorFloat(e->renderer, color.r, color.g, color.b, color.a);
            SDL_RenderFillRect(e->renderer, &r);
            SDL_SetRenderDrawColorFloat(e->renderer, r0, g0, b0, a0);
            e->resolved[i] = NULL;
            continue;
        }
        Glyph *g = op->cmd == TTF_DRAW_COMMAND_COPY
                       ? get_glyph(e, op->copy.glyph_font, op->copy.glyph_index, lcd) : NULL;
        e->resolved[i] = g && g->rect.w > 0 ? g : NULL;
        if (e->resolved[i]) pending++;
    }

    /* One draw per (atlas, kind), in order of first use. */
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    while (pending > 0) {
        Atlas *atlas = NULL;
        GlyphKind kind = GLYPH_ALPHA;
        int quads = 0;
        for (int i = 0; i < n; ++i) {
            Glyph *g = e->resolved[i];
            if (!g) continue;
            if (!atlas) { atlas = g->atlas; kind = g->kind; }
            else if (g->atlas != atlas || g->kind != kind) continue;
            const TTF_CopyOperation *c = &ops[i].copy;
            float x0, y0, w, h, u0, v0;
            if (g->kind == GLYPH_LCD) {
                /* Whole subpixel image, placed where the grayscale one's
                 * origin is (dst - src undoes SDL_ttf's clipping). */
                x0 = x + (float)(c->dst.x - c->src.x + g->dx);
                y0 = y + (float)(c->dst.y - c->src.y + g->dy);
                w = (float)g->rect.w;
                h = (float)g->rect.h;
                u0 = (float)g->rect.x;
                v0 = (float)g->rect.y;
            } else {
                x0 = x + (float)c->dst.x;
                y0 = y + (float)c->dst.y;
                w = (float)SDL_min(c->dst.w, g->rect.w - c->src.x);
                h = (float)SDL_min(c->dst.h, g->rect.h - c->src.y);
                u0 = (float)(g->rect.x + c->src.x);
                v0 = (float)(g->rect.y + c->src.y);
            }
            const float k = 1.0f / ATLAS_SIZE;
            float *p = e->xy + quads * 8, *t = e->uv + quads * 8;
            p[0] = x0;     p[1] = y0;     p[2] = x0 + w; p[3] = y0;
            p[4] = x0 + w; p[5] = y0 + h; p[6] = x0;     p[7] = y0 + h;
            t[0] = u0 * k;       t[1] = v0 * k;       t[2] = (u0 + w) * k; t[3] = v0 * k;
            t[4] = (u0 + w) * k; t[5] = (v0 + h) * k; t[6] = u0 * k;       t[7] = (v0 + h) * k;
            for (int j = 0; j < 6; ++j) e->indices[quads * 6 + j] = quads * 4 + order[j];
            quads++;
            e->resolved[i] = NULL;
            pending--;
        }
        draw_batch(e, atlas->texture, quads, kind, color);
    }
    return true;
}
