#include "chrome.h"
#include "text.h"
#include "fonts.h"

/* Layout of the bar, in logical pixels (times the DPI scale). The sizes
 * are those of the Windows 11 caption buttons. */
#define BAR_H 32.0f
#define BUTTON_W 46.0f
#define ICON_W 40.0f
#define RESIZE_BAND 5.0f /* top edge that still resizes (restored window) */

enum { BTN_NONE, BTN_MIN, BTN_MAX, BTN_CLOSE };

static SDL_Window *win;
static int hover, pressed; /* BTN_* under the mouse / held down */

static int button_px(float s) { return (int)(BUTTON_W * s + 0.5f); }
static int icon_px(float s) { return (int)(ICON_W * s + 0.5f); }

#ifdef _WIN32
static int bar_px(float s) { return (int)(BAR_H * s + 0.5f); }

/* The caption button at x in a bar w pixels wide, BTN_NONE if none. */
static int button_at(int x, int w, float s)
{
    const int bw = button_px(s);
    if (x >= w || x < w - 3 * bw) return BTN_NONE;
    return BTN_CLOSE - (w - 1 - x) / bw;
}

#include <windows.h>
#include <windowsx.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static HWND hwnd;
static WNDPROC sdl_proc;       /* SDL's window procedure, which we wrap */
static void (*on_changed)(void);
static bool tracking_leave;    /* asked for WM_NCMOUSELEAVE */

/* Only a normal captioned window gets the custom bar (SDL makes fullscreen
 * windows WS_POPUP). */
static bool custom_frame(HWND h)
{
    return (GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
}

static float dpi_scale(HWND h)
{
    UINT dpi = GetDpiForWindow(h);
    return dpi ? (float)dpi / 96.0f : 1.0f;
}

static void set_state(int new_hover, int new_pressed)
{
    if (new_hover == hover && new_pressed == pressed) return;
    hover = new_hover;
    pressed = new_pressed;
    if (on_changed) on_changed();
}

static int button_of_hit(WPARAM hit)
{
    switch (hit) {
    case HTMINBUTTON: return BTN_MIN;
    case HTMAXBUTTON: return BTN_MAX;
    case HTCLOSE:     return BTN_CLOSE;
    default:          return BTN_NONE;
    }
}

/* What is under screen point lp; def is DefWindowProc's answer, which
 * already covers the native left / right / bottom resize borders. */
static LRESULT hit_test(HWND h, LPARAM lp, LRESULT def)
{
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(h, &pt);
    const float s = dpi_scale(h);
    const bool zoomed = IsZoomed(h) != 0;
    const int band = zoomed ? 0 : (int)(RESIZE_BAND * s + 0.5f);
    if (def != HTCLIENT) {
        /* There is no native top border any more: its corners are ours. */
        if (pt.y < band && def == HTLEFT) return HTTOPLEFT;
        if (pt.y < band && def == HTRIGHT) return HTTOPRIGHT;
        return def;
    }
    if (pt.y < 0 || pt.y >= bar_px(s)) return HTCLIENT;
    if (pt.y < band) return HTTOP;
    RECT rc;
    GetClientRect(h, &rc);
    switch (button_at(pt.x, rc.right, s)) {
    case BTN_MIN:   return HTMINBUTTON;
    /* HTMAXBUTTON is what makes Windows 11 offer Snap Layouts on hover. */
    case BTN_MAX:   return HTMAXBUTTON;
    case BTN_CLOSE: return HTCLOSE;
    default: break;
    }
    if (pt.x < icon_px(s)) return HTSYSMENU;
    return HTCAPTION;
}

static LRESULT CALLBACK chrome_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp && custom_frame(h)) {
            /* Let Windows (via SDL) work out the normal client area, then
             * give it back the caption and the top border. The side and
             * bottom borders stay native: on Windows 10/11 they are the
             * invisible resize margins outside the visible window. */
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            const LONG top = p->rgrc[0].top;
            LRESULT res = CallWindowProcW(sdl_proc, h, msg, wp, lp);
            if (res != 0) return res;
            p->rgrc[0].top = top;
            if (IsZoomed(h)) {
                /* A maximized window hangs over the screen edges by its
                 * frame; keep the bar on screen. */
                UINT dpi = GetDpiForWindow(h);
                p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                  GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (custom_frame(h)) {
            LRESULT def = CallWindowProcW(sdl_proc, h, msg, wp, lp);
            return hit_test(h, lp, def);
        }
        break;
    case WM_NCMOUSEMOVE:
        if (!tracking_leave) {
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE | TME_NONCLIENT, h, 0};
            tracking_leave = TrackMouseEvent(&t) != 0;
        }
        set_state(button_of_hit(wp), pressed);
        break;
    case WM_NCMOUSELEAVE:
        tracking_leave = false;
        set_state(BTN_NONE, BTN_NONE);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        /* Ours to draw: keep DefWindowProc from running its classic
         * caption-button loop. */
        if (button_of_hit(wp)) {
            set_state(button_of_hit(wp), button_of_hit(wp));
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (button_of_hit(wp)) {
            const int b = button_of_hit(wp);
            if (b == pressed) {
                /* What the native buttons do, animations included. */
                WPARAM cmd = b == BTN_MIN ? SC_MINIMIZE
                           : b == BTN_CLOSE ? SC_CLOSE
                           : IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE;
                PostMessageW(h, WM_SYSCOMMAND, cmd, 0);
            }
            set_state(b, BTN_NONE);
            return 0;
        }
        if (pressed) set_state(hover, BTN_NONE);
        break;
    default:
        break;
    }
    return CallWindowProcW(sdl_proc, h, msg, wp, lp);
}

void chrome_set_dark(bool dark)
{
    if (!hwnd) return;
    typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    static SetAttr set_attr;
    static bool looked_up;
    if (!looked_up) {
        looked_up = true;
        /* Already loaded by SDL; no import (and no load before main()). */
        HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dwm) set_attr = (SetAttr)(void (*)(void))GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    if (!set_attr) return;
    BOOL on = dark;
    set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on);
}

void chrome_init(SDL_Window *window, bool dark, void (*changed)(void))
{
    win = window;
    on_changed = changed;
    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd) return;
    sdl_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)chrome_proc);
    if (!sdl_proc) {
        hwnd = NULL;
        return;
    }
    chrome_set_dark(dark);
    /* Recompute the frame now (WM_NCCALCSIZE), while still hidden. */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOOWNERZORDER | SWP_NOACTIVATE);
}

float chrome_height(void)
{
    if (!hwnd || !custom_frame(hwnd)) return 0;
    return (float)bar_px(dpi_scale(hwnd));
}

#else /* !_WIN32: keep the system's title bar */

void chrome_init(SDL_Window *window, bool dark, void (*changed)(void))
{
    (void)dark;
    (void)changed;
    win = window;
}
float chrome_height(void) { return 0; }
void chrome_set_dark(bool dark) { (void)dark; }

#endif

/* ------------------------------------------------------------ drawing -- */

/* Texts are kept between frames; most frames redraw the same bar. */
typedef struct CachedText {
    TTF_Text *text;
    TTF_Font *font;
    char str[1024];
} CachedText;

enum { TXT_ICON, TXT_TITLE, TXT_MIN, TXT_MAX, TXT_CLOSE, TXT_COUNT };
static CachedText texts[TXT_COUNT];

static TTF_Text *cached(int slot, TTF_TextEngine *engine, TTF_Font *font, const char *s)
{
    CachedText *c = &texts[slot];
    if (c->text && c->font == font && SDL_strcmp(c->str, s) == 0) return c->text;
    if (c->text) TTF_DestroyText(c->text);
    c->text = TTF_CreateText(engine, font, s, 0);
    c->font = font;
    SDL_strlcpy(c->str, s, sizeof c->str);
    return c->text;
}

static void draw_centered(TTF_Text *t, SDL_Color c, const SDL_FRect *box)
{
    if (!t) return;
    int w = 0, h = 0;
    TTF_GetTextSize(t, &w, &h);
    TTF_SetTextColor(t, c.r, c.g, c.b, c.a);
    md_text_draw(t, box->x + SDL_floorf((box->w - (float)w) * 0.5f),
                         box->y + SDL_floorf((box->h - (float)h) * 0.5f));
}

/* Codicons from the Nerd Font (the glyphs VS Code's title bar uses). */
#define GLYPH_MINIMIZE "\xEE\xAA\xBA" /* U+EABA chrome-minimize */
#define GLYPH_MAXIMIZE "\xEE\xAA\xB9" /* U+EAB9 chrome-maximize */
#define GLYPH_RESTORE  "\xEE\xAA\xBB" /* U+EABB chrome-restore */
#define GLYPH_CLOSE    "\xEE\xAA\xB8" /* U+EAB8 chrome-close */
#define GLYPH_MARKDOWN "\xEE\x9C\xBE" /* U+E73E dev-markdown */

/* md's own icon (the exe's icon resource), as a texture size pixels
 * square; NULL elsewhere or on failure. Loaded on first use. */
static SDL_Texture *icon_texture(SDL_Renderer *r, int size)
{
    static SDL_Texture *tex;
    static int tex_size;
    if (tex && tex_size == size) return tex;
    if (tex) SDL_DestroyTexture(tex);
    tex = NULL;
    tex_size = size;
#ifdef _WIN32
    /* MD_APP_ICON (1) holds 16, 32, 48, 64, 128 and 256 px images. */
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  size, size, LR_DEFAULTCOLOR);
    if (!ico) return NULL;
    ICONINFO info;
    if (GetIconInfo(ico, &info)) {
        SDL_Surface *surf = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_ARGB8888);
        HDC dc = GetDC(NULL);
        BITMAPINFO bi;
        SDL_zero(bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (surf && dc && info.hbmColor && surf->pitch == size * 4 &&
            GetDIBits(dc, info.hbmColor, 0, (UINT)size, surf->pixels, &bi, DIB_RGB_COLORS) == size) {
            tex = SDL_CreateTextureFromSurface(r, surf);
            /* Pixel art: keep the pixels square. */
            if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        }
        if (dc) ReleaseDC(NULL, dc);
        SDL_DestroySurface(surf);
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    DestroyIcon(ico);
#else
    (void)r;
#endif
    return tex;
}

/* The end of s, after "…", that fits in room pixels: a long path keeps its
 * file name. Returns s itself when it fits. */
static const char *fit_title(TTF_Font *font, const char *s, float room)
{
    /* Measured again only when the title, font or room changes. */
    static char buf[1024 + 4], last[1024];
    static TTF_Font *last_font;
    static float last_room = -1;
    if (font == last_font && room == last_room && SDL_strcmp(s, last) == 0) return buf;
    last_font = font;
    last_room = room;
    SDL_strlcpy(last, s, sizeof last);
    SDL_strlcpy(buf, s, sizeof buf);
    int w = 0, h = 0;
    if (!TTF_GetStringSize(font, s, 0, &w, &h) || (float)w <= room) return buf;
    for (const char *p = s + 1; *p; ++p) {
        if (((unsigned char)*p & 0xC0) == 0x80) continue; /* inside a UTF-8 sequence */
        SDL_snprintf(buf, sizeof buf, "\xE2\x80\xA6%s", p);
        if (TTF_GetStringSize(font, buf, 0, &w, &h) && (float)w <= room) return buf;
    }
    SDL_strlcpy(buf, "\xE2\x80\xA6", sizeof buf);
    return buf;
}

void chrome_draw(SDL_Renderer *r, TTF_TextEngine *engine, const Theme *th, const char *title,
                 bool active, float width, float height, float scale)
{
    if (height <= 0) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_FRect bar = {0, 0, width, height};
    SDL_SetRenderDrawColor(r, th->code_bg.r, th->code_bg.g, th->code_bg.b, 0xFF);
    SDL_RenderFillRect(r, &bar);
    const float line_h = SDL_max(1.0f, SDL_floorf(scale));
    SDL_FRect line = {0, height - line_h, width, line_h};
    SDL_SetRenderDrawColor(r, th->rule.r, th->rule.g, th->rule.b, 0xFF);
    SDL_RenderFillRect(r, &line);
    if (!engine) return; /* fonts not in yet: just the bar */

    const SDL_Color fg = active ? th->text : th->muted;
    const SDL_Color white = {0xFF, 0xFF, 0xFF, 0xFF};
    const float bw = (float)button_px(scale);
    const bool maximized = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED);
    /* Through Roboto: its Nerd Font fallback is unhinted, and hinting
     * flattens these thin-line icons. */
    TTF_Font *glyphs = fonts_get(FONT_REGULAR, 16.0f * scale);
    static const char *const GLYPH[] = {NULL, GLYPH_MINIMIZE, GLYPH_MAXIMIZE, GLYPH_CLOSE};

    for (int b = BTN_MIN; b <= BTN_CLOSE; ++b) {
        SDL_FRect box = {width - (float)(BTN_CLOSE - b + 1) * bw, 0, bw, height - line_h};
        const bool hot = hover == b || pressed == b;
        const bool down = pressed == b && hover == b;
        SDL_Color glyph_c = fg;
        if (b == BTN_CLOSE && hot) {
            /* The Windows 11 close-button red. */
            SDL_SetRenderDrawColor(r, 0xC4, 0x2B, 0x1C, down ? 0xE0 : 0xFF);
            SDL_RenderFillRect(r, &box);
            glyph_c = white;
        } else if (hot) {
            SDL_SetRenderDrawColor(r, th->text.r, th->text.g, th->text.b, down ? 0x2E : 0x17);
            SDL_RenderFillRect(r, &box);
        }
        if (!glyphs) continue;
        const char *g = b == BTN_MAX && maximized ? GLYPH_RESTORE : GLYPH[b];
        draw_centered(cached(TXT_MIN + b - BTN_MIN, engine, glyphs, g), glyph_c, &box);
    }
    if (!glyphs) return;

    const float icon_w = (float)icon_px(scale);
    SDL_FRect icon_box = {0, 0, icon_w, height - line_h};
    /* 16 px per whole step of scale: exact images from the icon, crisp. */
    const int icon_size = 16 * SDL_max(1, (int)(scale + 0.5f));
    SDL_Texture *icon = icon_texture(r, icon_size);
    TTF_Font *icon_font = icon ? NULL : fonts_get(FONT_REGULAR, 24.0f * scale);
    if (icon) {
        SDL_FRect dst = {SDL_floorf((icon_w - (float)icon_size) * 0.5f),
                         SDL_floorf((height - line_h - (float)icon_size) * 0.5f),
                         (float)icon_size, (float)icon_size};
        SDL_SetTextureAlphaModFloat(icon, active ? 1.0f : 0.6f);
        SDL_RenderTexture(r, icon, NULL, &dst);
    } else if (icon_font) {
        draw_centered(cached(TXT_ICON, engine, icon_font, GLYPH_MARKDOWN),
                      active ? th->accent : th->muted, &icon_box);
    }

    TTF_Font *title_font = fonts_get(FONT_REGULAR, 13.0f * scale);
    const float title_end = width - 3 * bw - 8 * scale;
    if (title_font && title_end > icon_w) {
        const char *full = title && title[0] ? title : "md";
        TTF_Text *t = cached(TXT_TITLE, engine, title_font,
                             fit_title(title_font, full, title_end - icon_w));
        if (t) {
            int tw = 0, tht = 0;
            TTF_GetTextSize(t, &tw, &tht);
            SDL_Rect clip = {(int)icon_w, 0, (int)(title_end - icon_w), (int)height};
            SDL_SetRenderClipRect(r, &clip);
            TTF_SetTextColor(t, fg.r, fg.g, fg.b, 0xFF);
            md_text_draw(t, icon_w, SDL_floorf((height - line_h - (float)tht) * 0.5f));
            SDL_SetRenderClipRect(r, NULL);
        }
    }
}
