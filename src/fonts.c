#include "fonts.h"
#include "assets.h"

#include <SDL3/SDL.h>

#define MAX_CACHED_FONTS 128

typedef struct FontEntry {
    FontFace face;
    float px;
    TTF_Font *font;
    TTF_Font *emoji; /* fallback, same size, shared between faces */
    TTF_Font *math;  /* symbol fallbacks, shared the same way */
    TTF_Font *symbols;
    TTF_Font *nerd;  /* Nerd Font icons for the proportional faces */
    bool owns_fallbacks;
} FontEntry;

static const int face_asset[FONT_FACE_COUNT] = {
    [FONT_REGULAR]     = ASSET_ROBOTO_REGULAR,
    [FONT_ITALIC]      = ASSET_ROBOTO_ITALIC,
    [FONT_MEDIUM]      = ASSET_ROBOTO_MEDIUM,
    [FONT_BOLD]        = ASSET_ROBOTO_BOLD,
    [FONT_BOLD_ITALIC] = ASSET_ROBOTO_BOLD_ITALIC,
    [FONT_LIGHT]       = ASSET_ROBOTO_LIGHT,
    [FONT_MONO]        = ASSET_ROBOTO_MONO,
    [FONT_MONO_ITALIC] = ASSET_ROBOTO_MONO_ITALIC,
    [FONT_MONO_BOLD]   = ASSET_ROBOTO_MONO_BOLD,
    [FONT_MONO_BOLD_ITALIC] = ASSET_ROBOTO_MONO_BOLD_ITALIC,
};

static FontEntry cache[MAX_CACHED_FONTS];
static int cache_count;

static TTF_Font *open_asset(int asset, float px)
{
    const void *data;
    size_t size;
    if (!asset_get(asset, &data, &size)) {
        SDL_SetError("embedded asset %d missing", asset);
        return NULL;
    }
    /* The IO stream only wraps the mapped bytes; FreeType reads tables lazily
     * straight from the executable image. */
    SDL_IOStream *io = SDL_IOFromConstMem(data, size);
    if (!io) return NULL;
    return TTF_OpenFontIO(io, true, px);
}

static TTF_Font *create_entry(FontFace face, float px)
{
    if (cache_count == MAX_CACHED_FONTS) {
        /* Should never happen in practice; fall back to the nearest size. */
        return cache[0].font;
    }
    TTF_Font *font = open_asset(face_asset[face], px);
    if (!font) return NULL;

    /* One set of fallback faces per size, shared by all text faces of that
     * size. Symbols come before emoji so arrows and check marks stay plain
     * text glyphs (emoji_cluster() routes real emoji to the emoji face). */
    TTF_Font *emoji = NULL, *math = NULL, *symbols = NULL, *nerd = NULL;
    bool owns = false;
    for (int i = 0; i < cache_count && !emoji; ++i) {
        if (cache[i].px == px && cache[i].emoji) {
            emoji = cache[i].emoji;
            math = cache[i].math;
            symbols = cache[i].symbols;
            nerd = cache[i].nerd;
        }
    }
    if (!emoji) {
        math = open_asset(ASSET_NOTO_MATH, px);
        symbols = open_asset(ASSET_NOTO_SYMBOLS2, px);
        nerd = open_asset(ASSET_ROBOTO_MONO, px);
        emoji = open_asset(ASSET_NOTO_EMOJI, px);
        owns = true;
    }
    if (math) TTF_AddFallbackFont(font, math);
    if (symbols) TTF_AddFallbackFont(font, symbols);
    bool mono = face == FONT_MONO || face == FONT_MONO_ITALIC || face == FONT_MONO_BOLD ||
                face == FONT_MONO_BOLD_ITALIC;
    if (nerd && !mono) TTF_AddFallbackFont(font, nerd);
    if (emoji) TTF_AddFallbackFont(font, emoji);

    FontEntry *e = &cache[cache_count++];
    e->face = face;
    e->px = px;
    e->font = font;
    e->emoji = emoji;
    e->math = math;
    e->symbols = symbols;
    e->nerd = nerd;
    e->owns_fallbacks = owns;
    return font;
}

bool fonts_init(float warm_px)
{
    if (!TTF_Init()) return false;
    /* Warm the body font (and with it FreeType, HarfBuzz and the emoji face)
     * so the first frame only has to rasterise glyphs. */
    return create_entry(FONT_REGULAR, warm_px) != NULL;
}

TTF_Font *fonts_get(FontFace face, float px)
{
    px = SDL_roundf(px * 4.0f) / 4.0f; /* avoid near-duplicate sizes */
    for (int i = 0; i < cache_count; ++i) {
        if (cache[i].face == face && cache[i].px == px) return cache[i].font;
    }
    TTF_Font *font = create_entry(face, px);
    if (!font && face != FONT_REGULAR) return fonts_get(FONT_REGULAR, px);
    return font;
}

TTF_Font *fonts_get_emoji(float px)
{
    px = SDL_roundf(px * 4.0f) / 4.0f;
    fonts_get(FONT_REGULAR, px);
    for (int i = 0; i < cache_count; ++i) {
        if (cache[i].face == FONT_REGULAR && cache[i].px == px) return cache[i].emoji;
    }
    return NULL;
}

void fonts_shutdown(void)
{
    for (int i = 0; i < cache_count; ++i) {
        TTF_CloseFont(cache[i].font);
    }
    for (int i = 0; i < cache_count; ++i) {
        if (!cache[i].owns_fallbacks) continue;
        if (cache[i].emoji) TTF_CloseFont(cache[i].emoji);
        if (cache[i].math) TTF_CloseFont(cache[i].math);
        if (cache[i].symbols) TTF_CloseFont(cache[i].symbols);
        if (cache[i].nerd) TTF_CloseFont(cache[i].nerd);
    }
    cache_count = 0;
    TTF_Quit();
}
