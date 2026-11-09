#include "clipboard.h"

#ifdef _WIN32
#include <windows.h>

/* Windows' "HTML Format" (CF_HTML): a header with byte offsets of the
 * document and of the fragment inside it. */
static char *make_cf_html(const char *html)
{
    static const char header[] =
        "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\n"
        "StartFragment:%010d\r\nEndFragment:%010d\r\n";
    static const char pre[] = "<html><body>\r\n<!--StartFragment-->";
    static const char post[] = "<!--EndFragment-->\r\n</body></html>";
    const int header_len = (int)SDL_strlen("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n"
                                           "StartFragment:0000000000\r\nEndFragment:0000000000\r\n");
    const int start_html = header_len;
    const int start_frag = start_html + (int)(sizeof pre - 1);
    const int end_frag = start_frag + (int)SDL_strlen(html);
    const int end_html = end_frag + (int)(sizeof post - 1);
    char *out = NULL;
    SDL_asprintf(&out, header, start_html, end_html, start_frag, end_frag);
    char *all = NULL;
    SDL_asprintf(&all, "%s%s%s%s", out ? out : "", pre, html, post);
    SDL_free(out);
    return all;
}

static bool put(UINT format, const void *data, size_t size)
{
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    void *p = GlobalLock(mem);
    if (!p) { GlobalFree(mem); return false; }
    SDL_memcpy(p, data, size);
    GlobalUnlock(mem);
    if (!SetClipboardData(format, mem)) { GlobalFree(mem); return false; }
    return true;
}

bool clipboard_set_rich(SDL_Window *window, const char *html, const char *text)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!OpenClipboard(hwnd)) return SDL_SetError("Could not open the clipboard");
    EmptyClipboard();

    /* Plain text: UTF-16 with CRLF line ends. */
    size_t n = 0;
    for (const char *p = text; *p; ++p) n += *p == '\n' ? 2 : 1;
    char *crlf = SDL_malloc(n + 1);
    char *w = crlf;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') *w++ = '\r';
        *w++ = *p;
    }
    *w = 0;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, crlf, -1, NULL, 0);
    bool ok = false;
    if (wlen > 0) {
        WCHAR *wide = SDL_malloc(sizeof(WCHAR) * (size_t)wlen);
        MultiByteToWideChar(CP_UTF8, 0, crlf, -1, wide, wlen);
        ok = put(CF_UNICODETEXT, wide, sizeof(WCHAR) * (size_t)wlen);
        SDL_free(wide);
    }
    SDL_free(crlf);

    char *cf_html = make_cf_html(html);
    if (cf_html) {
        UINT fmt = RegisterClipboardFormatW(L"HTML Format");
        ok = put(fmt, cf_html, SDL_strlen(cf_html) + 1) && ok;
        SDL_free(cf_html);
    }
    CloseClipboard();
    return ok ? true : SDL_SetError("Could not put the text on the clipboard");
}

#else

typedef struct Rich { char *html, *text; } Rich;

static const void *SDLCALL rich_data(void *userdata, const char *mime, size_t *size)
{
    Rich *r = userdata;
    const char *d = SDL_strncmp(mime, "text/html", 9) == 0 ? r->html : r->text;
    *size = SDL_strlen(d);
    return d;
}

static void SDLCALL rich_free(void *userdata)
{
    Rich *r = userdata;
    SDL_free(r->html);
    SDL_free(r->text);
    SDL_free(r);
}

bool clipboard_set_rich(SDL_Window *window, const char *html, const char *text)
{
    (void)window;
    static const char *mimes[] = {"text/html", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING"};
    Rich *r = SDL_malloc(sizeof *r);
    r->html = SDL_strdup(html);
    r->text = SDL_strdup(text);
    return SDL_SetClipboardData(rich_data, rich_free, r, mimes, SDL_arraysize(mimes));
}

#endif
