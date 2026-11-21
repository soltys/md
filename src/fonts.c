#include "fonts.h"
#include "assets.h"
#include "timing.h"

#include <SDL3/SDL.h>

#define MAX_CACHED_FONTS 128

typedef struct FontEntry {
    FontFace face;
    float px;
    TTF_Font *font;
    TTF_Font *math;  /* symbol fallbacks, same size, shared between faces */
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
    [FONT_SERIF]       = ASSET_SERIF_REGULAR,
    [FONT_SERIF_ITALIC] = ASSET_SERIF_ITALIC,
    [FONT_SERIF_MEDIUM] = ASSET_SERIF_MEDIUM,
    [FONT_SERIF_BOLD]  = ASSET_SERIF_BOLD,
    [FONT_SERIF_BOLD_ITALIC] = ASSET_SERIF_BOLD_ITALIC,
};

#if MD_TIMING
static const char *const face_name[FONT_FACE_COUNT] = {
    [FONT_REGULAR] = "Roboto Regular", [FONT_ITALIC] = "Roboto Italic",
    [FONT_MEDIUM] = "Roboto Medium", [FONT_BOLD] = "Roboto Bold",
    [FONT_BOLD_ITALIC] = "Roboto Bold Italic", [FONT_LIGHT] = "Roboto Light",
    [FONT_MONO] = "Roboto Mono", [FONT_MONO_ITALIC] = "Roboto Mono Italic",
    [FONT_MONO_BOLD] = "Roboto Mono Bold", [FONT_MONO_BOLD_ITALIC] = "Roboto Mono Bold Italic",
    [FONT_SERIF] = "Roboto Serif Regular", [FONT_SERIF_ITALIC] = "Roboto Serif Italic",
    [FONT_SERIF_MEDIUM] = "Roboto Serif Medium", [FONT_SERIF_BOLD] = "Roboto Serif Bold",
    [FONT_SERIF_BOLD_ITALIC] = "Roboto Serif Bold Italic",
};
#endif

static FontEntry cache[MAX_CACHED_FONTS];
static int cache_count;

/* Noto Color Emoji, one per size, opened only when some text needs it: it
 * is a 25 MB font that takes ~4 ms to open per size (and must be unpacked
 * first), and most documents have no emoji at all. */
#define MAX_EMOJI_SIZES 32
static struct { float px; TTF_Font *font; } emoji_cache[MAX_EMOJI_SIZES];
static int emoji_count;

static TTF_Font *open_asset(int asset, float px)
{
    const void *data;
    size_t size;
    if (!asset_get(asset, &data, &size)) {
        SDL_SetError("embedded asset %d missing", asset);
        return NULL;
    }
    /* The IO stream only wraps the bytes (the mapped executable, or the
     * unpacked copy of a compressed font); FreeType reads tables lazily. */
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
    /* Roboto Serif has no Greek (Roboto has): Roboto of the same size comes
     * first in a serif face's fallbacks. Opened before this entry so it is
     * cached (and owns the size's symbol fallbacks) on its own. */
    const bool serif = face >= FONT_SERIF && face <= FONT_SERIF_BOLD_ITALIC;
    TTF_Font *sans = serif ? fonts_get(FONT_REGULAR, px) : NULL;
    if (cache_count == MAX_CACHED_FONTS) return cache[0].font;
    TIMING_MARK("(work before font open)");
    TTF_Font *font = open_asset(face_asset[face], px);
    TIMING_MARK("open font %s %.2fpx", face_name[face], px);
    if (!font) return NULL;

    /* One set of fallback faces per size, shared by all text faces of that
     * size. Symbols come before emoji so arrows and check marks stay plain
     * text glyphs (emoji_cluster() routes real emoji to the emoji face). */
    TTF_Font *math = NULL, *symbols = NULL, *nerd = NULL;
    bool owns = true;
    for (int i = 0; i < cache_count; ++i) {
        if (cache[i].px == px && cache[i].owns_fallbacks) {
            math = cache[i].math;
            symbols = cache[i].symbols;
            nerd = cache[i].nerd;
            owns = false;
            break;
        }
    }
    if (owns) {
        math = open_asset(ASSET_NOTO_MATH, px);
        TIMING_MARK("open fallback Noto Sans Math %.2fpx", px);
        symbols = open_asset(ASSET_NOTO_SYMBOLS2, px);
        TIMING_MARK("open fallback Noto Sans Symbols 2 %.2fpx", px);
        nerd = open_asset(ASSET_ROBOTO_MONO, px);
        TIMING_MARK("open fallback Roboto Mono (Nerd icons) %.2fpx", px);
    }
    if (sans) TTF_AddFallbackFont(font, sans);
    if (math) TTF_AddFallbackFont(font, math);
    if (symbols) TTF_AddFallbackFont(font, symbols);
    bool mono = face == FONT_MONO || face == FONT_MONO_ITALIC || face == FONT_MONO_BOLD ||
                face == FONT_MONO_BOLD_ITALIC;
    if (nerd && !mono) TTF_AddFallbackFont(font, nerd);
    /* The emoji face joins the chain only once it has been opened for this
     * size (fonts_get_emoji), then for every face of that size. */
    for (int i = 0; i < emoji_count; ++i) {
        if (emoji_cache[i].px == px && emoji_cache[i].font) TTF_AddFallbackFont(font, emoji_cache[i].font);
    }
    TIMING_MARK("attach fallbacks to %s %.2fpx", face_name[face], px);

    FontEntry *e = &cache[cache_count++];
    e->face = face;
    e->px = px;
    e->font = font;
    e->math = math;
    e->symbols = symbols;
    e->nerd = nerd;
    e->owns_fallbacks = owns;
    return font;
}

static const int serif_assets[] = {
    ASSET_SERIF_REGULAR, ASSET_SERIF_BOLD, ASSET_SERIF_ITALIC,
    ASSET_SERIF_MEDIUM, ASSET_SERIF_BOLD_ITALIC,
};

bool fonts_init(float warm_px, bool serif)
{
    if (!TTF_Init()) return false;
    TIMING_MARK("TTF_Init (FreeType, HarfBuzz)");
    /* Warm the body font (and with it FreeType, HarfBuzz and the symbol
     * fallbacks) so the first frame only has to rasterise glyphs. */
    if (!create_entry(serif ? FONT_SERIF : FONT_REGULAR, warm_px)) return false;
    /* Unpack the other faces in the background, roughly in the order a
     * page asks for them, so the main thread finds them ready. The emoji
     * font is not among them: see fonts_prefetch_emoji(). Roboto Serif
     * only when it is the body font: otherwise it waits for F7. */
    static const int later[] = {
        ASSET_ROBOTO_BOLD, ASSET_ROBOTO_MEDIUM, ASSET_ROBOTO_ITALIC,
        ASSET_ROBOTO_MONO_BOLD, ASSET_ROBOTO_MONO_ITALIC, ASSET_ROBOTO_BOLD_ITALIC,
        ASSET_ROBOTO_MONO_BOLD_ITALIC, ASSET_ROBOTO_LIGHT,
    };
    static const int later_serif[] = {
        ASSET_SERIF_BOLD, ASSET_SERIF_ITALIC, ASSET_SERIF_MEDIUM,
        ASSET_ROBOTO_MONO_BOLD, ASSET_ROBOTO_MONO_ITALIC, ASSET_SERIF_BOLD_ITALIC,
        ASSET_ROBOTO_MONO_BOLD_ITALIC, ASSET_ROBOTO_REGULAR, ASSET_ROBOTO_MEDIUM,
        ASSET_ROBOTO_LIGHT, ASSET_ROBOTO_BOLD, ASSET_ROBOTO_ITALIC, ASSET_ROBOTO_BOLD_ITALIC,
    };
    if (serif) asset_prefetch(later_serif, (int)SDL_arraysize(later_serif));
    else asset_prefetch(later, (int)SDL_arraysize(later));
    TIMING_MARK("start unpacking the other faces");
    return true;
}

FontFace fonts_body_face(FontFace face, bool serif)
{
    if (!serif) return face;
    switch (face) {
    case FONT_REGULAR:     return FONT_SERIF;
    case FONT_ITALIC:      return FONT_SERIF_ITALIC;
    case FONT_MEDIUM:      return FONT_SERIF_MEDIUM;
    case FONT_BOLD:        return FONT_SERIF_BOLD;
    case FONT_BOLD_ITALIC: return FONT_SERIF_BOLD_ITALIC;
    default:               return face;
    }
}

void fonts_prefetch_serif(void)
{
    asset_prefetch(serif_assets, (int)SDL_arraysize(serif_assets));
}

void fonts_prefetch_emoji(void)
{
    static const int emoji[] = { ASSET_NOTO_EMOJI };
    asset_prefetch(emoji, 1);
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
    for (int i = 0; i < emoji_count; ++i) {
        if (emoji_cache[i].px == px) return emoji_cache[i].font;
    }
    if (emoji_count == MAX_EMOJI_SIZES) return emoji_cache[0].font;
    TIMING_MARK("(work before font open)");
    TTF_Font *emoji = open_asset(ASSET_NOTO_EMOJI, px);
    /* An event: emoji are often first needed after the first render. */
    TIMING_EVENT("open Noto Color Emoji %.2fpx (first emoji at this size)", px);
    /* Cached even when it failed, so a missing font is not retried for
     * every emoji. */
    emoji_cache[emoji_count].px = px;
    emoji_cache[emoji_count].font = emoji;
    emoji_count++;
    /* Last in the fallback chain of every face of this size. */
    for (int i = 0; emoji && i < cache_count; ++i) {
        if (cache[i].px == px) TTF_AddFallbackFont(cache[i].font, emoji);
    }
    return emoji;
}

void fonts_shutdown(void)
{
    for (int i = 0; i < cache_count; ++i) {
        TTF_CloseFont(cache[i].font);
    }
    for (int i = 0; i < emoji_count; ++i) {
        if (emoji_cache[i].font) TTF_CloseFont(emoji_cache[i].font);
    }
    emoji_count = 0;
    for (int i = 0; i < cache_count; ++i) {
        if (!cache[i].owns_fallbacks) continue;
        if (cache[i].math) TTF_CloseFont(cache[i].math);
        if (cache[i].symbols) TTF_CloseFont(cache[i].symbols);
        if (cache[i].nerd) TTF_CloseFont(cache[i].nerd);
    }
    cache_count = 0;
    TTF_Quit();
}
