#ifndef MD_FONTS_H
#define MD_FONTS_H

#include <stdbool.h>
#include <SDL3_ttf/SDL_ttf.h>

typedef enum FontFace {
    FONT_REGULAR,
    FONT_ITALIC,
    FONT_MEDIUM,
    FONT_BOLD,
    FONT_BOLD_ITALIC,
    FONT_LIGHT,
    FONT_MONO,
    FONT_MONO_ITALIC,
    FONT_MONO_BOLD,
    FONT_MONO_BOLD_ITALIC,
    FONT_FACE_COUNT
} FontFace;

/* Initialises SDL_ttf and opens the faces needed for the first frame.
 * Safe to call from a worker thread before SDL_Init(); the main thread must
 * not touch any other fonts_* function until this has returned. */
bool fonts_init(float warm_px);

/* Main thread only. Returns a font for the face at the given pixel size,
 * with the symbol fonts attached as fallbacks (and Noto Color Emoji once
 * fonts_get_emoji() has opened it for that size). Fonts are cached forever. */
TTF_Font *fonts_get(FontFace face, float px);

/* Noto Color Emoji at the given size (main thread only). Opened on the
 * first call for a size, so only ask when there really is an emoji. */
TTF_Font *fonts_get_emoji(float px);

/* Any thread: starts unpacking the emoji font in the background (the
 * loader calls this for documents that contain emoji). */
void fonts_prefetch_emoji(void);

void fonts_shutdown(void);

#endif
