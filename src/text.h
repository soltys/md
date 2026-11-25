#ifndef MD_TEXT_H
#define MD_TEXT_H

/* md's own SDL_ttf text engine: a glyph atlas like SDL_ttf's renderer text
 * engine, plus ClearType-style subpixel (LCD) glyphs drawn in two passes
 * with custom blend modes, so each colour channel of a pixel gets its own
 * coverage. Falls back to SDL_ttf's grayscale glyphs where the renderer has
 * no custom blending (the software renderer), when Windows has ClearType
 * off, or with MD_FONT_AA=grayscale. */

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

/* Called by fonts.c for every font it opens: the font file's bytes (alive
 * as long as the font), its size and hinting, so subpixel glyphs can be
 * rasterised from the same outlines SDL_ttf lays out. Unregistered fonts
 * (the colour emoji font) always get SDL_ttf's own glyph images. */
void md_text_register_font(TTF_Font *font, const void *data, size_t size, float px,
                           TTF_HintingFlags hinting);

TTF_TextEngine *md_text_engine_create(SDL_Renderer *renderer);
void md_text_engine_destroy(TTF_TextEngine *engine);

/* Whether text is drawn with subpixel glyphs now; false while grayscale is
 * forced (PNG export) or unsupported. */
bool md_text_subpixel(TTF_TextEngine *engine);
/* Forces grayscale glyphs (true) or restores the default (false). */
void md_text_force_grayscale(TTF_TextEngine *engine, bool gray);

/* Replaces TTF_DrawRendererText for texts made with md_text_engine_create. */
bool md_text_draw(TTF_Text *text, float x, float y);

#endif
