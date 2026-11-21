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
    /* Roboto Serif: the optional serif body font (fonts_body_face). */
    FONT_SERIF,
    FONT_SERIF_ITALIC,
    FONT_SERIF_MEDIUM,
    FONT_SERIF_BOLD,
    FONT_SERIF_BOLD_ITALIC,
    FONT_FACE_COUNT
} FontFace;

/* Initialises SDL_ttf and opens the faces needed for the first frame
 * (Roboto Serif instead of Roboto for the body text when serif is set).
 * Safe to call from a worker thread before SDL_Init(); the main thread must
 * not touch any other fonts_* function until this has returned. */
bool fonts_init(float warm_px, bool serif);

/* The serif counterpart of a proportional Roboto face (Regular, Italic,
 * Medium, Bold, Bold Italic) when serif is set; any other face as it is. */
FontFace fonts_body_face(FontFace face, bool serif);

/* Any thread: starts unpacking the Roboto Serif faces in the background
 * (called when the reader switches to serif). */
void fonts_prefetch_serif(void);

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
