#include "fonts.h"
#include "assets.h"

#include <SDL3/SDL.h>

#define MAX_CACHED_FONTS 64

typedef struct FontEntry {
    FontFace face;
    float px;
    TTF_Font *font;
    TTF_Font *emoji; /* fallback, same size */
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

    TTF_Font *emoji = open_asset(ASSET_NOTO_EMOJI, px);
    if (emoji) TTF_AddFallbackFont(font, emoji);

    FontEntry *e = &cache[cache_count++];
    e->face = face;
    e->px = px;
    e->font = font;
    e->emoji = emoji;
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

void fonts_shutdown(void)
{
    for (int i = 0; i < cache_count; ++i) {
        TTF_CloseFont(cache[i].font);
        if (cache[i].emoji) TTF_CloseFont(cache[i].emoji);
    }
    cache_count = 0;
    TTF_Quit();
}
