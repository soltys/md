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
 * with Noto Color Emoji attached as fallback. Fonts are cached forever. */
TTF_Font *fonts_get(FontFace face, float px);

void fonts_shutdown(void);

#endif
