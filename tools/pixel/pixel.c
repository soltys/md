/*
 * pixel: a small pixel art editor limited to the PICO-8 16 colour palette.
 * Used to draw md's icons and artwork. Saves plain PNGs (transparent
 * background) and exports scaled PNGs and Windows .ico files.
 *
 *   pixel.exe                 new 16x16 canvas
 *   pixel.exe art.png         open art.png (created on first save if missing)
 *   pixel.exe art.png 32x32   new canvas of that size when art.png is missing
 *
 * Press F1 inside the window for the key list.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MAX_DIM     256
#define NCOLORS     16
#define TRANSPARENT 16 /* palette index used for "no pixel" */
#define UNDO_MAX    128

/* The PICO-8 palette, 0xRRGGBB. */
static const uint32_t PAL[NCOLORS] = {
    0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
    0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA,
};
static const char *const PAL_NAMES[NCOLORS + 1] = {
    "black", "dark blue", "dark purple", "dark green", "brown", "dark grey",
    "light grey", "white", "red", "orange", "yellow", "green", "blue",
    "lavender", "pink", "light peach", "transparent",
};

enum { T_PENCIL, T_LINE, T_RECT, T_FILL, T_PICK, T_COUNT };
static const char *const TOOL_NAMES[T_COUNT] = {"pencil", "line", "rect", "fill", "picker"};
static const char TOOL_KEYS[T_COUNT] = {'B', 'L', 'R', 'F', 'I'};

enum { MIRROR_X = 1, MIRROR_Y = 2 };
static const char *const MIRROR_NAMES[4] = {"off", "L|R", "T/B", "both"};

enum { DLG_OPEN, DLG_SAVE };

static const int SIZES[] = {8, 16, 32, 64, 128, 256};
#define NSIZES ((int)(sizeof SIZES / sizeof SIZES[0]))

typedef struct {
    int w, h;
    uint8_t *px; /* w*h palette indices, TRANSPARENT for empty */
} Image;

typedef struct { float x, y, w, h; } Box;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex, *checker;
    int tex_w, tex_h;
    bool tex_dirty;

    Image img;
    Image undo[UNDO_MAX];
    int undo_n;
    Image redo[UNDO_MAX];
    int redo_n;
    Image base; /* canvas at stroke start, for line/rect previews */

    int tool, fg, bg, mirror;
    bool grid, help, dirty, dirty_before;

    /* view */
    int zoom;
    float ox, oy; /* screen position of canvas pixel (0,0) */
    bool fit;

    /* pointer state */
    bool drawing, filled;
    int draw_color, sx, sy, lx, ly;
    bool panning, space;
    float pan_mx, pan_my, pan_ox, pan_oy;
    float mx, my;

    char path[1024];
    char msg[256];
    Uint64 msg_until;
    bool quit, quit_after_save;
    Uint32 dialog_event;
    float u; /* UI scale */
    int out_w, out_h;
} S;

/* ------------------------------------------------------------ images --- */

static bool img_alloc(Image *im, int w, int h)
{
    uint8_t *px = SDL_malloc((size_t)w * (size_t)h);
    if (!px) return false;
    memset(px, TRANSPARENT, (size_t)w * (size_t)h);
    im->w = w;
    im->h = h;
    im->px = px;
    return true;
}

static void img_free(Image *im)
{
    SDL_free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

static bool img_copy(Image *dst, const Image *src)
{
    uint8_t *px = SDL_malloc((size_t)src->w * (size_t)src->h);
    if (!px) return false;
    memcpy(px, src->px, (size_t)src->w * (size_t)src->h);
    img_free(dst);
    dst->w = src->w;
    dst->h = src->h;
    dst->px = px;
    return true;
}

static bool img_equal(const Image *a, const Image *b)
{
    return a->w == b->w && a->h == b->h &&
           memcmp(a->px, b->px, (size_t)a->w * (size_t)a->h) == 0;
}

/* ---------------------------------------------------------- messages --- */

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(S.msg, sizeof S.msg, fmt, ap);
    va_end(ap);
    S.msg_until = SDL_GetTicks() + 3500;
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *c = p; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

static void update_title(void)
{
    char t[1200];
    SDL_snprintf(t, sizeof t, "%s%s - pixel (%dx%d)", S.dirty ? "* " : "",
                 S.path[0] ? base_name(S.path) : "untitled", S.img.w, S.img.h);
    SDL_SetWindowTitle(S.win, t);
}

static void set_dirty(bool d)
{
    if (S.dirty != d) {
        S.dirty = d;
        update_title();
    }
}

/* -------------------------------------------------------------- undo --- */

static void stack_push(Image *stack, int *n, const Image *im)
{
    if (*n == UNDO_MAX) {
        img_free(&stack[0]);
        memmove(stack, stack + 1, sizeof(Image) * (UNDO_MAX - 1));
        stack[UNDO_MAX - 1].px = NULL;
        (*n)--;
    }
    Image c = {0};
    if (img_copy(&c, im)) stack[(*n)++] = c;
}

static void clear_stack(Image *stack, int *n)
{
    while (*n > 0) img_free(&stack[--(*n)]);
}

/* Snapshot before a change. */
static void push_undo(void)
{
    stack_push(S.undo, &S.undo_n, &S.img);
    clear_stack(S.redo, &S.redo_n);
}

/* Drop the snapshot again if the change turned out to be a no-op. */
static void commit_undo(void)
{
    if (S.undo_n > 0 && img_equal(&S.undo[S.undo_n - 1], &S.img)) {
        img_free(&S.undo[--S.undo_n]);
        return;
    }
    set_dirty(true);
}

static void after_size_change(void)
{
    S.fit = true;
    S.tex_dirty = true;
    update_title();
}

static void undo_redo(bool redo)
{
    Image *from = redo ? S.redo : S.undo, *to = redo ? S.undo : S.redo;
    int *from_n = redo ? &S.redo_n : &S.undo_n, *to_n = redo ? &S.undo_n : &S.redo_n;
    if (*from_n == 0) {
        status(redo ? "nothing to redo" : "nothing to undo");
        return;
    }
    bool resized;
    stack_push(to, to_n, &S.img);
    Image prev = from[--(*from_n)];
    resized = prev.w != S.img.w || prev.h != S.img.h;
    img_free(&S.img);
    S.img = prev;
    S.tex_dirty = true;
    if (resized) after_size_change();
    set_dirty(true);
}

/* ----------------------------------------------------------- drawing --- */

static void put(int x, int y, int c)
{
    if (x >= 0 && y >= 0 && x < S.img.w && y < S.img.h)
        S.img.px[y * S.img.w + x] = (uint8_t)c;
}

static void plot(int x, int y, int c)
{
    int mx = S.img.w - 1 - x, my = S.img.h - 1 - y;
    put(x, y, c);
    if (S.mirror & MIRROR_X) put(mx, y, c);
    if (S.mirror & MIRROR_Y) put(x, my, c);
    if (S.mirror == (MIRROR_X | MIRROR_Y)) put(mx, my, c);
    S.tex_dirty = true;
}

static void line(int x0, int y0, int x1, int y1, int c)
{
    int dx = SDL_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -SDL_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rect(int x0, int y0, int x1, int y1, int c, bool filled)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (filled || x == x0 || x == x1 || y == y0 || y == y1) plot(x, y, c);
}

static void flood(int x, int y, int c)
{
    if (x < 0 || y < 0 || x >= S.img.w || y >= S.img.h) return;
    int w = S.img.w, h = S.img.h;
    uint8_t target = S.img.px[y * w + x];
    if (target == c) return;
    int *stack = SDL_malloc(sizeof(int) * (size_t)w * (size_t)h * 4 + sizeof(int));
    if (!stack) return;
    int n = 0;
    stack[n++] = y * w + x;
    while (n > 0) {
        int i = stack[--n];
        if (S.img.px[i] != target) continue;
        S.img.px[i] = (uint8_t)c;
        int px = i % w, py = i / w;
        if (px > 0) stack[n++] = i - 1;
        if (px < w - 1) stack[n++] = i + 1;
        if (py > 0) stack[n++] = i - w;
        if (py < h - 1) stack[n++] = i + w;
    }
    SDL_free(stack);
    S.tex_dirty = true;
}

static void fill(int x, int y, int c)
{
    flood(x, y, c);
    if (S.mirror & MIRROR_X) flood(S.img.w - 1 - x, y, c);
    if (S.mirror & MIRROR_Y) flood(x, S.img.h - 1 - y, c);
    if (S.mirror == (MIRROR_X | MIRROR_Y)) flood(S.img.w - 1 - x, S.img.h - 1 - y, c);
}

/* Shapes are redrawn from the stroke's starting canvas on every move. */
static void draw_shape(int x, int y)
{
    memcpy(S.img.px, S.base.px, (size_t)S.img.w * (size_t)S.img.h);
    if (S.tool == T_LINE) line(S.sx, S.sy, x, y, S.draw_color);
    else rect(S.sx, S.sy, x, y, S.draw_color, S.filled);
    S.tex_dirty = true;
}

static void resize_canvas(int w, int h)
{
    Image n = {0};
    if (w == S.img.w && h == S.img.h) return;
    if (!img_alloc(&n, w, h)) return;
    push_undo();
    for (int y = 0; y < SDL_min(h, S.img.h); y++)
        memcpy(n.px + y * w, S.img.px + y * S.img.w, (size_t)SDL_min(w, S.img.w));
    img_free(&S.img);
    S.img = n;
    set_dirty(true);
    after_size_change();
    status("canvas %dx%d", w, h);
}

static void shift_canvas(int dx, int dy)
{
    Image n = {0};
    if (!img_alloc(&n, S.img.w, S.img.h)) return;
    push_undo();
    int w = S.img.w, h = S.img.h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            n.px[((y + dy + h) % h) * w + (x + dx + w) % w] = S.img.px[y * w + x];
    img_free(&S.img);
    S.img = n;
    S.tex_dirty = true;
    commit_undo();
}

static void flip_canvas(bool horizontal)
{
    push_undo();
    int w = S.img.w, h = S.img.h;
    for (int y = 0; y < (horizontal ? h : h / 2); y++)
        for (int x = 0; x < (horizontal ? w / 2 : w); x++) {
            int a = y * w + x;
            int b = horizontal ? y * w + (w - 1 - x) : (h - 1 - y) * w + x;
            uint8_t t = S.img.px[a];
            S.img.px[a] = S.img.px[b];
            S.img.px[b] = t;
        }
    S.tex_dirty = true;
    commit_undo();
}

/* ----------------------------------------------------------- file I/O --- */

static int nearest_color(Uint8 r, Uint8 g, Uint8 b)
{
    int best = 0, best_d = 0x7fffffff;
    for (int i = 0; i < NCOLORS; i++) {
        int dr = (int)((PAL[i] >> 16) & 0xff) - r;
        int dg = (int)((PAL[i] >> 8) & 0xff) - g;
        int db = (int)(PAL[i] & 0xff) - b;
        /* weighted RGB distance, close enough to perceptual for 16 colours */
        int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Loads a PNG (or anything SDL_image reads) and maps it to the palette. */
static bool load_image(const char *path, Image *out)
{
    SDL_Surface *s = IMG_Load(path);
    if (!s) {
        status("can't open %s: %s", base_name(path), SDL_GetError());
        return false;
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) {
        status("can't read %s: %s", base_name(path), SDL_GetError());
        return false;
    }
    if (c->w > MAX_DIM || c->h > MAX_DIM) {
        status("%s is %dx%d; pixel edits images up to %dx%d", base_name(path), c->w, c->h,
               MAX_DIM, MAX_DIM);
        SDL_DestroySurface(c);
        return false;
    }
    Image im = {0};
    if (!img_alloc(&im, c->w, c->h)) {
        SDL_DestroySurface(c);
        return false;
    }
    int remapped = 0;
    for (int y = 0; y < c->h; y++) {
        const Uint8 *row = (const Uint8 *)c->pixels + (size_t)y * (size_t)c->pitch;
        for (int x = 0; x < c->w; x++) {
            const Uint8 *p = row + x * 4;
            if (p[3] < 128) continue;
            int i = nearest_color(p[0], p[1], p[2]);
            if (PAL[i] != (((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2])) remapped++;
            im.px[y * c->w + x] = (uint8_t)i;
        }
    }
    SDL_DestroySurface(c);
    *out = im;
    if (remapped) status("opened %s (%d pixels mapped to PICO-8 colours)", base_name(path), remapped);
    else status("opened %s", base_name(path));
    return true;
}

/* RGBA surface of the image scaled up by an integer factor. */
static SDL_Surface *to_surface(int scale)
{
    SDL_Surface *s = SDL_CreateSurface(S.img.w * scale, S.img.h * scale, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    for (int y = 0; y < s->h; y++) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * (size_t)s->pitch;
        for (int x = 0; x < s->w; x++) {
            int i = S.img.px[(y / scale) * S.img.w + x / scale];
            Uint8 *p = row + x * 4;
            if (i == TRANSPARENT) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                p[0] = (Uint8)(PAL[i] >> 16);
                p[1] = (Uint8)(PAL[i] >> 8);
                p[2] = (Uint8)PAL[i];
                p[3] = 255;
            }
        }
    }
    return s;
}

static bool save_png(const char *path, int scale)
{
    SDL_Surface *s = to_surface(scale);
    bool ok = s && IMG_SavePNG(s, path);
    if (!ok) status("can't save %s: %s", base_name(path), SDL_GetError());
    SDL_DestroySurface(s);
    return ok;
}

static void put_le16(Uint8 *p, unsigned v) { p[0] = (Uint8)v; p[1] = (Uint8)(v >> 8); }
static void put_le32(Uint8 *p, Uint32 v)
{
    put_le16(p, v & 0xffff);
    put_le16(p + 2, v >> 16);
}

/* A Windows .ico holding PNG-compressed images at every standard icon size
   that is a whole multiple of the canvas (so pixels stay square and crisp). */
static bool save_ico(const char *path, int *count)
{
    static const int ico_sizes[] = {16, 24, 32, 48, 64, 128, 256};
    SDL_IOStream *pngs[8];
    int dims[8], n = 0;
    *count = 0;
    if (S.img.w != S.img.h) {
        status("icons need a square canvas");
        return false;
    }
    for (int i = 0; i < (int)(sizeof ico_sizes / sizeof ico_sizes[0]); i++) {
        int d = ico_sizes[i];
        if (d < S.img.w || d % S.img.w) continue;
        SDL_Surface *s = to_surface(d / S.img.w);
        SDL_IOStream *io = SDL_IOFromDynamicMem();
        if (!s || !io || !IMG_SavePNG_IO(s, io, false)) {
            SDL_DestroySurface(s);
            if (io) SDL_CloseIO(io);
            continue;
        }
        SDL_DestroySurface(s);
        pngs[n] = io;
        dims[n++] = d;
    }
    if (n == 0) {
        status("no icon size is a multiple of %d", S.img.w);
        return false;
    }
    bool ok = false;
    SDL_IOStream *f = SDL_IOFromFile(path, "wb");
    if (f) {
        Uint8 hdr[6], ent[16];
        put_le16(hdr, 0);
        put_le16(hdr + 2, 1); /* icon */
        put_le16(hdr + 4, (unsigned)n);
        ok = SDL_WriteIO(f, hdr, 6) == 6;
        Uint32 offset = 6 + 16 * (Uint32)n;
        for (int i = 0; i < n && ok; i++) {
            Uint32 len = (Uint32)SDL_GetIOSize(pngs[i]);
            memset(ent, 0, sizeof ent);
            ent[0] = (Uint8)(dims[i] == 256 ? 0 : dims[i]);
            ent[1] = ent[0];
            put_le16(ent + 4, 1);  /* planes */
            put_le16(ent + 6, 32); /* bpp */
            put_le32(ent + 8, len);
            put_le32(ent + 12, offset);
            offset += len;
            ok = SDL_WriteIO(f, ent, 16) == 16;
        }
        for (int i = 0; i < n && ok; i++) {
            SDL_PropertiesID props = SDL_GetIOProperties(pngs[i]);
            void *mem = SDL_GetPointerProperty(props, SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
            size_t len = (size_t)SDL_GetIOSize(pngs[i]);
            ok = mem && SDL_WriteIO(f, mem, len) == len;
        }
        ok = SDL_CloseIO(f) && ok;
    }
    for (int i = 0; i < n; i++) SDL_CloseIO(pngs[i]);
    if (!ok) status("can't write %s: %s", base_name(path), SDL_GetError());
    *count = n;
    return ok;
}

static bool has_png_ext(const char *p)
{
    size_t n = SDL_strlen(p);
    return n >= 4 && SDL_strcasecmp(p + n - 4, ".png") == 0;
}

static void do_save(const char *path)
{
    char p[sizeof S.path];
    SDL_snprintf(p, sizeof p, "%s%s", path, has_png_ext(path) ? "" : ".png");
    if (!save_png(p, 1)) {
        S.quit_after_save = false;
        return;
    }
    SDL_strlcpy(S.path, p, sizeof S.path);
    S.dirty = false;
    update_title();
    status("saved %s", base_name(p));
    if (S.quit_after_save) S.quit = true;
}

/* Writes <name>@8x.png and, for square canvases, <name>.ico next to the file. */
static void do_export(void)
{
    if (!S.path[0]) {
        status("save first (Ctrl+S), exports go next to the PNG");
        return;
    }
    char stem[sizeof S.path], out[sizeof S.path + 16];
    SDL_strlcpy(stem, S.path, sizeof stem);
    if (has_png_ext(stem)) stem[SDL_strlen(stem) - 4] = 0;

    SDL_snprintf(out, sizeof out, "%s@8x.png", stem);
    if (!save_png(out, 8)) return;
    char done[300];
    SDL_snprintf(done, sizeof done, "exported %s", base_name(out));
    if (S.img.w == S.img.h) {
        int n;
        SDL_snprintf(out, sizeof out, "%s.ico", stem);
        if (!save_ico(out, &n)) return;
        SDL_snprintf(done + SDL_strlen(done), sizeof done - SDL_strlen(done),
                     " and %s (%d sizes)", base_name(out), n);
    }
    status("%s", done);
}

static void open_path(const char *path)
{
    Image im = {0};
    if (!load_image(path, &im)) return;
    img_free(&S.img);
    S.img = im;
    clear_stack(S.undo, &S.undo_n);
    clear_stack(S.redo, &S.redo_n);
    SDL_strlcpy(S.path, path, sizeof S.path);
    S.dirty = false;
    after_size_change();
}

static const SDL_DialogFileFilter PNG_FILTER[] = {{"PNG images", "png"}};

static void SDLCALL dialog_done(void *ud, const char *const *list, int filter)
{
    (void)filter;
    SDL_Event e;
    SDL_zero(e);
    e.type = S.dialog_event;
    e.user.code = (Sint32)(intptr_t)ud;
    e.user.data1 = (list && list[0]) ? SDL_strdup(list[0]) : NULL;
    SDL_PushEvent(&e);
}

static void save_as(void)
{
    SDL_ShowSaveFileDialog(dialog_done, (void *)(intptr_t)DLG_SAVE, S.win, PNG_FILTER, 1,
                           S.path[0] ? S.path : NULL);
}

static void save(void)
{
    if (S.path[0]) do_save(S.path);
    else save_as();
}

/* Asks about unsaved changes. Returns true when the caller may go ahead. */
static bool confirm_discard(const char *what, bool quitting)
{
    if (!S.dirty) return true;
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Save"},
        {0, 1, "Discard"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Cancel"},
    };
    char text[200];
    SDL_snprintf(text, sizeof text, "Save changes before %s?", what);
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_WARNING, S.win, "pixel", text,
                                     3, buttons, NULL};
    int choice = 2;
    if (!SDL_ShowMessageBox(&data, &choice)) return false;
    if (choice == 1) return true;
    if (choice == 0) {
        if (!S.path[0]) {
            /* finish once the save dialog returns (quitting only) */
            S.quit_after_save = quitting;
            save_as();
            return false;
        }
        do_save(S.path);
        return !S.dirty;
    }
    return false;
}

/* ------------------------------------------------------------- layout --- */

static float text_scale(void) { return SDL_max(1.0f, SDL_roundf(2 * S.u)); }
static float swatch(void) { return SDL_roundf(50 * S.u); }
static float pad(void) { return SDL_roundf(10 * S.u); }
static float gap(void) { return SDL_roundf(4 * S.u); }
static float line_h(void) { return SDL_roundf(24 * S.u); }
static float status_h(void) { return SDL_roundf(30 * S.u); }

/* PICO-8 style 4x4 palette; the panel is as wide as the palette and fits
   the longest label ("M mirror both", 13 glyphs). */
static float panel_width(void)
{
    float palette = 4 * swatch() + 3 * gap();
    float labels = 13 * 8 * text_scale() + 2 * gap();
    return SDL_max(palette, labels) + 2 * pad();
}

static Box color_box(int i)
{
    Box b;
    if (i == TRANSPARENT) {
        b.x = pad();
        b.y = pad() + 4 * (swatch() + gap());
        b.w = panel_width() - 2 * pad();
        b.h = SDL_roundf(swatch() * 0.5f);
        return b;
    }
    b.x = pad() + (float)(i % 4) * (swatch() + gap());
    b.y = pad() + (float)(i / 4) * (swatch() + gap());
    b.w = b.h = swatch();
    return b;
}

static float current_y(void)
{
    Box t = color_box(TRANSPARENT);
    return t.y + t.h + pad();
}

/* Rows below the fg/bg preview: tools, then mirror and grid toggles. */
static float rows_y(void) { return current_y() + swatch() * 1.5f + pad(); }
static Box row_box(int row)
{
    Box b = {pad() - gap(), rows_y() + (float)row * line_h(), panel_width() - 2 * pad() + 2 * gap(), line_h()};
    return b;
}
#define ROW_MIRROR T_COUNT
#define ROW_GRID   (T_COUNT + 1)
#define NROWS      (T_COUNT + 2)

static bool in_box(Box b, float x, float y)
{
    return x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h;
}

static void layout(void)
{
    SDL_GetCurrentRenderOutputSize(S.ren, &S.out_w, &S.out_h);
    if (!S.fit) return;
    float aw = (float)S.out_w - panel_width(), ah = (float)S.out_h - status_h();
    int z = (int)SDL_floorf(SDL_min(aw * 0.85f / (float)S.img.w, ah * 0.85f / (float)S.img.h));
    S.zoom = SDL_max(z, 1);
    S.ox = SDL_floorf(panel_width() + (aw - (float)(S.img.w * S.zoom)) / 2);
    S.oy = SDL_floorf((ah - (float)(S.img.h * S.zoom)) / 2);
}

static bool screen_to_pixel(float x, float y, int *px, int *py)
{
    *px = (int)SDL_floorf((x - S.ox) / (float)S.zoom);
    *py = (int)SDL_floorf((y - S.oy) / (float)S.zoom);
    return *px >= 0 && *py >= 0 && *px < S.img.w && *py < S.img.h;
}

static bool over_canvas_area(float x, float y)
{
    return x >= panel_width() && y < (float)S.out_h - status_h();
}

static void zoom_at(float x, float y, int dir)
{
    int z = S.zoom;
    if (dir > 0) z += SDL_max(1, z / 4);
    else z -= SDL_max(1, z / 5);
    z = SDL_clamp(z, 1, 128);
    if (z == S.zoom) return;
    float cx = (x - S.ox) / (float)S.zoom, cy = (y - S.oy) / (float)S.zoom;
    S.ox = SDL_floorf(x - cx * (float)z);
    S.oy = SDL_floorf(y - cy * (float)z);
    S.zoom = z;
    S.fit = false;
}

/* ------------------------------------------------------------ render --- */

static void set_color(uint32_t rgb, Uint8 a)
{
    SDL_SetRenderDrawColor(S.ren, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, a);
}

static void fill_box(Box b, uint32_t rgb)
{
    SDL_FRect r = {b.x, b.y, b.w, b.h};
    set_color(rgb, 255);
    SDL_RenderFillRect(S.ren, &r);
}

static void outline(Box b, float t, uint32_t rgb)
{
    set_color(rgb, 255);
    SDL_FRect r[4] = {
        {b.x - t, b.y - t, b.w + 2 * t, t},
        {b.x - t, b.y + b.h, b.w + 2 * t, t},
        {b.x - t, b.y, t, b.h},
        {b.x + b.w, b.y, t, b.h},
    };
    SDL_RenderFillRects(S.ren, r, 4);
}

static void text(float x, float y, uint32_t rgb, const char *s)
{
    float k = text_scale();
    SDL_SetRenderScale(S.ren, k, k);
    set_color(rgb, 255);
    SDL_RenderDebugText(S.ren, SDL_roundf(x / k), SDL_roundf(y / k), s);
    SDL_SetRenderScale(S.ren, 1, 1);
}

static void draw_checker(Box b)
{
    float c = SDL_max(4, SDL_roundf(b.h / 4));
    fill_box(b, 0x2A2A33);
    set_color(0x3C3C47, 255);
    for (float y = 0; y < b.h; y += c)
        for (float x = (SDL_fmodf(y / c, 2) < 1 ? 0 : c); x < b.w; x += 2 * c) {
            SDL_FRect r = {b.x + x, b.y + y, SDL_min(c, b.w - x), SDL_min(c, b.h - y)};
            SDL_RenderFillRect(S.ren, &r);
        }
}

static void draw_swatch(Box b, int i)
{
    if (i == TRANSPARENT) draw_checker(b);
    else fill_box(b, PAL[i]);
}

static void update_textures(void)
{
    if (!S.tex || S.tex_w != S.img.w || S.tex_h != S.img.h) {
        SDL_DestroyTexture(S.tex);
        SDL_DestroyTexture(S.checker);
        S.tex = SDL_CreateTexture(S.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                  S.img.w, S.img.h);
        /* 2x2 checker cells per canvas pixel */
        SDL_Surface *cs = SDL_CreateSurface(S.img.w * 2, S.img.h * 2, SDL_PIXELFORMAT_RGBA32);
        if (cs) {
            for (int y = 0; y < cs->h; y++) {
                Uint8 *row = (Uint8 *)cs->pixels + (size_t)y * (size_t)cs->pitch;
                for (int x = 0; x < cs->w; x++) {
                    Uint8 v = ((x ^ y) & 1) ? 0x3C : 0x2A;
                    row[x * 4 + 0] = v;
                    row[x * 4 + 1] = v;
                    row[x * 4 + 2] = (Uint8)(v + 10);
                    row[x * 4 + 3] = 255;
                }
            }
            S.checker = SDL_CreateTextureFromSurface(S.ren, cs);
            SDL_DestroySurface(cs);
        }
        SDL_SetTextureScaleMode(S.tex, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureScaleMode(S.checker, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(S.tex, SDL_BLENDMODE_BLEND);
        S.tex_w = S.img.w;
        S.tex_h = S.img.h;
        S.tex_dirty = true;
    }
    if (!S.tex_dirty || !S.tex) return;
    static Uint8 buf[MAX_DIM * MAX_DIM * 4];
    for (int i = 0; i < S.img.w * S.img.h; i++) {
        int c = S.img.px[i];
        Uint8 *p = buf + i * 4;
        if (c == TRANSPARENT) {
            p[0] = p[1] = p[2] = p[3] = 0;
        } else {
            p[0] = (Uint8)(PAL[c] >> 16);
            p[1] = (Uint8)(PAL[c] >> 8);
            p[2] = (Uint8)PAL[c];
            p[3] = 255;
        }
    }
    SDL_UpdateTexture(S.tex, NULL, buf, S.img.w * 4);
    S.tex_dirty = false;
}

static void draw_grid(void)
{
    if (!S.grid || S.zoom < 6) return;
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_BLEND);
    float W = (float)(S.img.w * S.zoom), H = (float)(S.img.h * S.zoom);
    for (int x = 1; x < S.img.w; x++) {
        SDL_FRect r = {S.ox + (float)(x * S.zoom), S.oy, 1, H};
        SDL_SetRenderDrawColor(S.ren, 255, 255, 255, (x % 8) ? 28 : 70);
        SDL_RenderFillRect(S.ren, &r);
    }
    for (int y = 1; y < S.img.h; y++) {
        SDL_FRect r = {S.ox, S.oy + (float)(y * S.zoom), W, 1};
        SDL_SetRenderDrawColor(S.ren, 255, 255, 255, (y % 8) ? 28 : 70);
        SDL_RenderFillRect(S.ren, &r);
    }
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_NONE);
}

static void draw_canvas(void)
{
    SDL_FRect dst = {S.ox, S.oy, (float)(S.img.w * S.zoom), (float)(S.img.h * S.zoom)};
    Box frame = {dst.x, dst.y, dst.w, dst.h};
    outline(frame, 1, 0x50505C);
    SDL_RenderTexture(S.ren, S.checker, NULL, &dst);
    SDL_RenderTexture(S.ren, S.tex, NULL, &dst);
    draw_grid();

    /* mirror axes */
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(S.ren, 0x29, 0xAD, 0xFF, 160);
    if (S.mirror & MIRROR_X) {
        SDL_FRect r = {dst.x + dst.w / 2 - 1, dst.y, 2, dst.h};
        SDL_RenderFillRect(S.ren, &r);
    }
    if (S.mirror & MIRROR_Y) {
        SDL_FRect r = {dst.x, dst.y + dst.h / 2 - 1, dst.w, 2};
        SDL_RenderFillRect(S.ren, &r);
    }
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_NONE);

    /* hovered pixel */
    int px, py;
    if (!S.panning && over_canvas_area(S.mx, S.my) && screen_to_pixel(S.mx, S.my, &px, &py)) {
        Box h = {S.ox + (float)(px * S.zoom), S.oy + (float)(py * S.zoom), (float)S.zoom, (float)S.zoom};
        outline(h, SDL_max(1, SDL_roundf(S.u)), 0xFFF1E8);
    }
}

static void draw_panel(void)
{
    Box p = {0, 0, panel_width(), (float)S.out_h - status_h()};
    fill_box(p, 0x1E1E26);

    for (int i = 0; i <= TRANSPARENT; i++) {
        Box b = color_box(i);
        draw_swatch(b, i);
        if (i == S.fg) outline(b, SDL_roundf(2 * S.u), 0xFFF1E8);
        else if (i == S.bg) outline(b, SDL_roundf(2 * S.u), 0x83769C);
    }

    /* foreground over background, like most paint programs */
    float s = swatch(), y = current_y();
    Box bg = {pad() + s * 0.6f, y + s * 0.5f, s, s};
    Box fg = {pad(), y, s, s};
    draw_swatch(bg, S.bg);
    outline(bg, 1, 0x50505C);
    draw_swatch(fg, S.fg);
    outline(fg, 1, 0xC2C3C7);
    text(pad() + s * 1.9f, y + s * 0.15f, 0xC2C3C7, "L/R");
    text(pad() + s * 1.9f, y + s * 0.15f + text_scale() * 10, 0x83769C, "X swap");

    char label[64];
    for (int r = 0; r < NROWS; r++) {
        Box b = row_box(r);
        bool active = r < T_COUNT && r == S.tool;
        if (active) fill_box(b, 0x34344A);
        if (r < T_COUNT)
            SDL_snprintf(label, sizeof label, "%c %s", TOOL_KEYS[r], TOOL_NAMES[r]);
        else if (r == ROW_MIRROR)
            SDL_snprintf(label, sizeof label, "M mirror %s", MIRROR_NAMES[S.mirror]);
        else
            SDL_snprintf(label, sizeof label, "G grid %s", S.grid ? "on" : "off");
        text(b.x + gap(), b.y + (b.h - 8 * text_scale()) / 2, active ? 0xFFF1E8 : 0xC2C3C7, label);
    }
    text(pad(), row_box(NROWS).y + gap(), 0x5F574F, "F1 help");
}

static void draw_status(void)
{
    Box b = {0, (float)S.out_h - status_h(), (float)S.out_w, status_h()};
    fill_box(b, 0x14141A);
    char s[400];
    int px, py;
    char pos[32] = "";
    if (over_canvas_area(S.mx, S.my) && screen_to_pixel(S.mx, S.my, &px, &py))
        SDL_snprintf(pos, sizeof pos, "  %d,%d", px, py);
    if (S.msg[0] && SDL_GetTicks() < S.msg_until)
        SDL_snprintf(s, sizeof s, "%s", S.msg);
    else
        SDL_snprintf(s, sizeof s, "%dx%d  %dx  %s  %s / %s%s", S.img.w, S.img.h, S.zoom,
                     TOOL_NAMES[S.tool], PAL_NAMES[S.fg], PAL_NAMES[S.bg], pos);
    text(pad(), b.y + (b.h - 8 * text_scale()) / 2, 0xC2C3C7, s);
}

static const char *const HELP[] = {
    "pixel - PICO-8 palette pixel art",
    "",
    "left / right mouse  draw with colour 1 / 2",
    "click palette       pick colour 1 / 2",
    "[ ]  X              prev/next colour, swap",
    "B L R F I           pencil line rect fill picker",
    "Shift (rect)        filled rectangle",
    "Alt+click           pick colour from canvas",
    "M  G                mirror mode, grid",
    "N  Shift+N          canvas size up / down",
    "Shift+arrows        shift image (wraps)",
    "H  V                flip horizontal / vertical",
    "wheel, Ctrl +/-     zoom      Home  fit",
    "middle drag, Space  pan",
    "Ctrl+Z  Ctrl+Y      undo / redo",
    "Ctrl+N              clear canvas",
    "Ctrl+O  drop file   open PNG",
    "Ctrl+S  Ctrl+Sh+S   save / save as PNG",
    "Ctrl+E              export @8x PNG + .ico",
    "",
    "F1 or Esc closes this",
};

static void draw_help(void)
{
    int n = (int)(sizeof HELP / sizeof HELP[0]);
    float k = text_scale(), lh = 12 * k, w = 50 * 8 * k + 2 * pad(), h = (float)n * lh + 2 * pad();
    Box b = {SDL_floorf(((float)S.out_w - w) / 2), SDL_floorf(((float)S.out_h - h) / 2), w, h};
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(S.ren, 0x10, 0x10, 0x16, 235);
    SDL_FRect r = {b.x, b.y, b.w, b.h};
    SDL_RenderFillRect(S.ren, &r);
    SDL_SetRenderDrawBlendMode(S.ren, SDL_BLENDMODE_NONE);
    outline(b, 2, 0x29ADFF);
    for (int i = 0; i < n; i++)
        text(b.x + pad(), b.y + pad() + (float)i * lh, i == 0 ? 0xFFEC27 : 0xC2C3C7, HELP[i]);
}

static void render(void)
{
    layout();
    update_textures();
    SDL_SetRenderDrawColor(S.ren, 0x18, 0x18, 0x20, 255);
    SDL_RenderClear(S.ren);
    draw_canvas();
    draw_panel();
    draw_status();
    if (S.help) draw_help();
    SDL_RenderPresent(S.ren);
}

/* ------------------------------------------------------------- input --- */

static void cycle_size(int dir)
{
    int cur = SDL_max(S.img.w, S.img.h), i;
    if (dir > 0) {
        for (i = 0; i < NSIZES && SIZES[i] <= cur; i++) {}
        if (i == NSIZES) return;
    } else {
        for (i = NSIZES - 1; i >= 0 && SIZES[i] >= cur; i--) {}
        if (i < 0) return;
        if (S.img.w != S.img.h) i = SDL_min(i + 1, NSIZES - 1);
    }
    resize_canvas(SIZES[i], SIZES[i]);
}

static void pick_at(int x, int y, bool secondary)
{
    int c = S.img.px[y * S.img.w + x];
    if (secondary) S.bg = c;
    else S.fg = c;
}

static void mouse_down(const SDL_MouseButtonEvent *e)
{
    bool secondary = e->button == SDL_BUTTON_RIGHT;
    if (S.help) {
        S.help = false;
        return;
    }
    if (e->button == SDL_BUTTON_MIDDLE || (S.space && e->button == SDL_BUTTON_LEFT)) {
        S.panning = true;
        S.pan_mx = e->x;
        S.pan_my = e->y;
        S.pan_ox = S.ox;
        S.pan_oy = S.oy;
        return;
    }
    if (e->button != SDL_BUTTON_LEFT && !secondary) return;

    if (e->x < panel_width()) {
        for (int i = 0; i <= TRANSPARENT; i++)
            if (in_box(color_box(i), e->x, e->y)) {
                if (secondary) S.bg = i;
                else S.fg = i;
                return;
            }
        for (int r = 0; r < NROWS; r++)
            if (in_box(row_box(r), e->x, e->y)) {
                if (r < T_COUNT) S.tool = r;
                else if (r == ROW_MIRROR) S.mirror = (S.mirror + 1) & 3;
                else S.grid = !S.grid;
                return;
            }
        return;
    }

    int x, y;
    if (!over_canvas_area(e->x, e->y)) return;
    bool inside = screen_to_pixel(e->x, e->y, &x, &y);
    if ((S.tool == T_PICK || (SDL_GetModState() & SDL_KMOD_ALT))) {
        if (inside) pick_at(x, y, secondary);
        return;
    }
    if (!inside && S.tool != T_LINE && S.tool != T_RECT) return;

    S.draw_color = secondary ? S.bg : S.fg;
    push_undo();
    if (S.tool == T_FILL) {
        fill(x, y, S.draw_color);
        commit_undo();
        return;
    }
    S.drawing = true;
    S.sx = S.lx = x;
    S.sy = S.ly = y;
    S.filled = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    SDL_CaptureMouse(true);
    if (S.tool == T_PENCIL) plot(x, y, S.draw_color);
    else {
        img_copy(&S.base, &S.img);
        draw_shape(x, y);
    }
}

static void mouse_move(const SDL_MouseMotionEvent *e)
{
    S.mx = e->x;
    S.my = e->y;
    if (S.panning) {
        S.ox = SDL_floorf(S.pan_ox + e->x - S.pan_mx);
        S.oy = SDL_floorf(S.pan_oy + e->y - S.pan_my);
        S.fit = false;
        return;
    }
    if (!S.drawing) return;
    int x, y;
    screen_to_pixel(e->x, e->y, &x, &y);
    if (S.tool == T_PENCIL) {
        if (x != S.lx || y != S.ly) line(S.lx, S.ly, x, y, S.draw_color);
    } else {
        S.filled = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        draw_shape(x, y);
    }
    S.lx = x;
    S.ly = y;
}

static void mouse_up(void)
{
    if (S.panning) {
        S.panning = false;
        return;
    }
    if (!S.drawing) return;
    S.drawing = false;
    SDL_CaptureMouse(false);
    commit_undo();
}

static void key_down(const SDL_KeyboardEvent *e)
{
    bool ctrl = (e->mod & SDL_KMOD_CTRL) != 0, shift = (e->mod & SDL_KMOD_SHIFT) != 0;
    if (S.drawing) return;
    if (S.help && (e->key == SDLK_ESCAPE || e->key == SDLK_F1)) {
        S.help = false;
        return;
    }
    if (ctrl) {
        switch (e->key) {
        case SDLK_Z: undo_redo(shift); break;
        case SDLK_Y: undo_redo(true); break;
        case SDLK_S: shift ? save_as() : save(); break;
        case SDLK_O:
            if (confirm_discard("opening another file", false))
                SDL_ShowOpenFileDialog(dialog_done, (void *)(intptr_t)DLG_OPEN, S.win, PNG_FILTER,
                                       1, S.path[0] ? S.path : NULL, false);
            break;
        case SDLK_E: do_export(); break;
        case SDLK_N:
            push_undo();
            memset(S.img.px, TRANSPARENT, (size_t)S.img.w * (size_t)S.img.h);
            S.tex_dirty = true;
            commit_undo();
            break;
        case SDLK_Q: if (confirm_discard("quitting", true)) S.quit = true; break;
        case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
            zoom_at(S.ox + (float)(S.img.w * S.zoom) / 2, S.oy + (float)(S.img.h * S.zoom) / 2, 1);
            break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
            zoom_at(S.ox + (float)(S.img.w * S.zoom) / 2, S.oy + (float)(S.img.h * S.zoom) / 2, -1);
            break;
        default: break;
        }
        return;
    }
    switch (e->key) {
    case SDLK_B: S.tool = T_PENCIL; break;
    case SDLK_L: S.tool = T_LINE; break;
    case SDLK_R: S.tool = T_RECT; break;
    case SDLK_F: S.tool = T_FILL; break;
    case SDLK_I: S.tool = T_PICK; break;
    case SDLK_X: { int t = S.fg; S.fg = S.bg; S.bg = t; } break;
    case SDLK_LEFTBRACKET: S.fg = (S.fg + NCOLORS) % (NCOLORS + 1); break;
    case SDLK_RIGHTBRACKET: S.fg = (S.fg + 1) % (NCOLORS + 1); break;
    case SDLK_M: S.mirror = (S.mirror + 1) & 3; status("mirror: %s", MIRROR_NAMES[S.mirror]); break;
    case SDLK_G: S.grid = !S.grid; break;
    case SDLK_N: cycle_size(shift ? -1 : 1); break;
    case SDLK_H: flip_canvas(true); break;
    case SDLK_V: flip_canvas(false); break;
    case SDLK_HOME: S.fit = true; break;
    case SDLK_SPACE: S.space = true; break;
    case SDLK_F1: S.help = !S.help; break;
    case SDLK_ESCAPE: S.help = false; break;
    case SDLK_LEFT: if (shift) shift_canvas(-1, 0); break;
    case SDLK_RIGHT: if (shift) shift_canvas(1, 0); break;
    case SDLK_UP: if (shift) shift_canvas(0, -1); break;
    case SDLK_DOWN: if (shift) shift_canvas(0, 1); break;
    case SDLK_SLASH: if (shift) S.help = !S.help; break;
    default: break;
    }
}

static void update_scale(void)
{
    float s = SDL_GetWindowDisplayScale(S.win);
    S.u = SDL_max(1.0f, SDL_roundf(s * 2) / 2); /* steps of 0.5 */
}

static bool parse_size(const char *s, int *w, int *h)
{
    char *end;
    long a = SDL_strtol(s, &end, 10), b = a;
    if (end == s) return false;
    if (*end == 'x' || *end == 'X') {
        const char *t = end + 1;
        b = SDL_strtol(t, &end, 10);
        if (end == t) return false;
    }
    if (*end || a < 1 || b < 1 || a > MAX_DIM || b > MAX_DIM) return false;
    *w = (int)a;
    *h = (int)b;
    return true;
}

int main(int argc, char **argv)
{
    int w = 16, h = 16;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!parse_size(argv[i], &w, &h)) file = argv[i];
    }

    SDL_SetAppMetadata("pixel", "0.1.0", "md.pixel");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "pixel", SDL_GetError(), NULL);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_CreateWindowAndRenderer("pixel", 1100, 760,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &S.win, &S.ren)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "pixel", SDL_GetError(), NULL);
        return 1;
    }
    S.dialog_event = SDL_RegisterEvents(1);
    S.fg = 7;  /* white */
    S.bg = TRANSPARENT;
    S.grid = true;
    S.fit = true;
    S.zoom = 1;
    update_scale();

    if (file && SDL_GetPathInfo(file, NULL)) {
        open_path(file);
    } else if (file) {
        SDL_strlcpy(S.path, file, sizeof S.path);
        status("new file %s (created on first save)", base_name(file));
    }
    if (!S.img.px && !img_alloc(&S.img, w, h)) return 1;
    after_size_change();

    bool redraw = true;
    while (!S.quit) {
        if (redraw) {
            render();
            redraw = false;
        }
        SDL_Event e;
        /* sleep until something happens; wake to clear an expiring message */
        Uint64 now = SDL_GetTicks();
        Sint32 timeout = S.msg_until > now ? (Sint32)(S.msg_until - now + 10) : -1;
        if (!SDL_WaitEventTimeout(&e, timeout)) {
            redraw = true;
            continue;
        }
        do {
            redraw = true;
            switch (e.type) {
            case SDL_EVENT_QUIT:
                if (confirm_discard("quitting", true)) S.quit = true;
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: update_scale(); S.fit = true; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_WHEEL:
                SDL_ConvertEventToRenderCoordinates(S.ren, &e);
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_down(&e.button);
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) mouse_up();
                else if (e.type == SDL_EVENT_MOUSE_MOTION) mouse_move(&e.motion);
                else if (e.wheel.y != 0 && over_canvas_area(e.wheel.mouse_x, e.wheel.mouse_y))
                    zoom_at(e.wheel.mouse_x, e.wheel.mouse_y, e.wheel.y > 0 ? 1 : -1);
                break;
            case SDL_EVENT_KEY_DOWN: key_down(&e.key); break;
            case SDL_EVENT_KEY_UP: if (e.key.key == SDLK_SPACE) S.space = false; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: S.space = false; break;
            case SDL_EVENT_DROP_FILE:
                if (confirm_discard("opening another file", false)) open_path(e.drop.data);
                break;
            default:
                if (e.type == S.dialog_event) {
                    char *p = e.user.data1;
                    if (!p) S.quit_after_save = false;
                    else if (e.user.code == DLG_OPEN) open_path(p);
                    else do_save(p);
                    SDL_free(p);
                }
                break;
            }
        } while (!S.quit && SDL_PollEvent(&e));
    }

    SDL_Quit();
    return 0;
}
