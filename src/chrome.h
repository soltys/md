#ifndef MD_CHROME_H
#define MD_CHROME_H

/* The window's own title bar ("chrome"), drawn by md in the theme's
 * colours instead of the generic Windows caption.
 *
 * Windows keeps doing the window management: the window stays a normal
 * captioned, resizable window, md only moves the client area up over the
 * system caption (WM_NCCALCSIZE) and tells Windows what is under the mouse
 * (WM_NCHITTEST). So dragging, Aero Snap, double-click to maximize, the
 * system menu, the Windows 11 Snap Layouts flyout on the maximize button,
 * rounded corners, the shadow and the resize borders all stay native.
 *
 * On other platforms (the Linux sanity build) the system title bar stays
 * and chrome_height() is 0. */

#include <stdbool.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "theme.h"

/* Takes over the title bar of a (still hidden) window. changed() is called
 * on the main thread, from inside the window procedure, when the bar must
 * be redrawn (a button hovered or pressed); it should only post an event. */
void chrome_init(SDL_Window *window, bool dark, void (*changed)(void));

/* Height of the bar in pixels, at the window's current DPI; 0 when the
 * window has no custom title bar (other platforms, fullscreen). */
float chrome_height(void);

/* Light / dark window frame (border, shadow, system menu). */
void chrome_set_dark(bool dark);

/* Draws the bar across the top of the window, height pixels tall. title is
 * the document name (or NULL); active is whether the window has focus. */
void chrome_draw(SDL_Renderer *r, TTF_TextEngine *engine, const Theme *th, const char *title,
                 bool active, float width, float height, float scale);

#endif
