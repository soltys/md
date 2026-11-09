#ifndef MD_CLIPBOARD_H
#define MD_CLIPBOARD_H

#include <SDL3/SDL.h>

/* Puts both an HTML fragment and its plain-text version on the clipboard,
 * so rich editors (Word, Outlook, browsers) paste the formatting and plain
 * ones paste the text. False (with SDL_GetError) on failure. */
bool clipboard_set_rich(SDL_Window *window, const char *html, const char *text);

#endif
