/*
 * pixelart: turns text sprite sheets drawn with the PICO-8 palette into PNG
 * assets for md. No editor, no window: edit the text, run the tool, commit
 * the PNGs.
 *
 *   pixelart.exe [-o outdir] sheet.txt [more.txt ...]
 *
 * Sheet format (see art/sprites.txt):
 *
 *   # comment
 *   sprite md-icon x1 x2 x4      <- name, then the scales to write
 *   .1111.                        <- one row per line, all rows equal width
 *   11cc11                           0-9 a-f = PICO-8 colour, . = transparent
 *   ...
 *                                 <- a blank line (or the next sprite) ends it
 *
 * Each scale writes one PNG: x1 -> name.png, x4 -> name@4x.png. Pixels are
 * scaled by plain repetition, so they stay hard-edged.
 */
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_DIM    256
#define MAX_SCALES 8

/* The PICO-8 palette, 0xRRGGBB. */
static const uint32_t PAL[16] = {
    0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
    0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA,
};

typedef struct {
    char name[128];
    int scales[MAX_SCALES], nscales;
    int w, h;
    int8_t px[MAX_DIM * MAX_DIM]; /* -1 = transparent */
    const char *file;
    int line;
} Sprite;

static const char *outdir = ".";
static int written, errors;

static void fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    errors++;
}

static int color_of(char c)
{
    if (c == '.') return -1;
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -2;
}

static bool write_png(const Sprite *s, int scale)
{
    char path[1024];
    if (scale == 1) snprintf(path, sizeof path, "%s/%s.png", outdir, s->name);
    else snprintf(path, sizeof path, "%s/%s@%dx.png", outdir, s->name, scale);

    SDL_Surface *surf = SDL_CreateSurface(s->w * scale, s->h * scale, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        fail(s->file, s->line, "%s: %s", s->name, SDL_GetError());
        return false;
    }
    for (int y = 0; y < surf->h; y++) {
        Uint8 *row = (Uint8 *)surf->pixels + (size_t)y * (size_t)surf->pitch;
        for (int x = 0; x < surf->w; x++) {
            int c = s->px[(y / scale) * s->w + x / scale];
            Uint8 *p = row + x * 4;
            if (c < 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                p[0] = (Uint8)(PAL[c] >> 16);
                p[1] = (Uint8)(PAL[c] >> 8);
                p[2] = (Uint8)PAL[c];
                p[3] = 255;
            }
        }
    }
    bool ok = IMG_SavePNG(surf, path);
    SDL_DestroySurface(surf);
    if (!ok) {
        fail(s->file, s->line, "can't write %s: %s", path, SDL_GetError());
        return false;
    }
    printf("%s (%dx%d)\n", path, s->w * scale, s->h * scale);
    written++;
    return true;
}

static void finish(Sprite *s)
{
    if (!s->name[0]) return;
    if (s->h == 0) fail(s->file, s->line, "sprite '%s' has no rows", s->name);
    else
        for (int i = 0; i < s->nscales; i++) write_png(s, s->scales[i]);
    s->name[0] = 0;
}

static bool valid_name(const char *n)
{
    if (!*n) return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') ||
              (*n >= '0' && *n <= '9') || *n == '-' || *n == '_'))
            return false;
    return true;
}

static void start(Sprite *s, char *args, const char *file, int line)
{
    char *save = NULL, *tok = SDL_strtok_r(args, " \t", &save);
    memset(s, 0, sizeof *s);
    s->file = file;
    s->line = line;
    if (!tok || !valid_name(tok)) {
        fail(file, line, "expected 'sprite <name> [x1 x2 ...]' (name: letters, digits, - _)");
        return;
    }
    SDL_strlcpy(s->name, tok, sizeof s->name);
    while ((tok = SDL_strtok_r(NULL, " \t", &save)) != NULL) {
        char *end = tok;
        long k = (tok[0] == 'x' || tok[0] == 'X') ? SDL_strtol(tok + 1, &end, 10) : 0;
        if (k < 1 || k > 64 || *end) {
            fail(file, line, "bad scale '%s' (use x1 .. x64)", tok);
            s->name[0] = 0;
            return;
        }
        if (s->nscales == MAX_SCALES) {
            fail(file, line, "at most %d scales per sprite", MAX_SCALES);
            s->name[0] = 0;
            return;
        }
        s->scales[s->nscales++] = (int)k;
    }
    if (s->nscales == 0) s->scales[s->nscales++] = 1;
}

static void add_row(Sprite *s, const char *row, const char *file, int line)
{
    int w = (int)strlen(row);
    if (s->h == 0) {
        if (w > MAX_DIM) {
            fail(file, line, "rows are limited to %d pixels", MAX_DIM);
            s->name[0] = 0;
            return;
        }
        s->w = w;
    } else if (w != s->w) {
        fail(file, line, "row is %d pixels wide, '%s' started with %d", w, s->name, s->w);
        s->name[0] = 0;
        return;
    }
    if (s->h == MAX_DIM) {
        fail(file, line, "sprites are limited to %d rows", MAX_DIM);
        s->name[0] = 0;
        return;
    }
    for (int x = 0; x < w; x++) {
        int c = color_of(row[x]);
        if (c == -2) {
            fail(file, line, "column %d: '%c' is not a PICO-8 colour (0-9, a-f) or '.'", x + 1, row[x]);
            s->name[0] = 0;
            return;
        }
        s->px[s->h * MAX_DIM + x] = (int8_t)c;
    }
    s->h++;
}

/* Rows are stored with a MAX_DIM stride while parsing; pack them. */
static void pack(Sprite *s)
{
    for (int y = 1; y < s->h; y++) memmove(s->px + y * s->w, s->px + y * MAX_DIM, (size_t)s->w);
}

static void process(const char *file)
{
    size_t size;
    char *text = SDL_LoadFile(file, &size);
    if (!text) {
        fprintf(stderr, "%s: error: %s\n", file, SDL_GetError());
        errors++;
        return;
    }
    static Sprite s;
    s.name[0] = 0;
    bool skipping = false; /* rows of a sprite whose header failed */
    int line = 0;
    /* strtok would merge blank lines, so split by hand */
    for (char *p = text; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line++;
        char *l = p;
        p = nl ? nl + 1 : NULL;

        size_t n = strlen(l);
        while (n && (l[n - 1] == '\r' || l[n - 1] == ' ' || l[n - 1] == '\t')) l[--n] = 0;
        while (*l == ' ' || *l == '\t') l++;

        if (*l == '#') continue;
        if (*l == 0) {
            if (s.name[0]) { pack(&s); finish(&s); }
            skipping = false;
            continue;
        }
        if (strncmp(l, "sprite", 6) == 0 && (l[6] == ' ' || l[6] == '\t' || l[6] == 0)) {
            if (s.name[0]) { pack(&s); finish(&s); }
            start(&s, l + 6, file, line);
            skipping = !s.name[0];
            continue;
        }
        if (s.name[0]) add_row(&s, l, file, line);
        else if (!skipping)
            fail(file, line, "pixel row outside a sprite (start one with 'sprite <name>')");
        /* after an error, skip the rest of this block quietly */
        skipping = !s.name[0];
    }
    if (s.name[0]) { pack(&s); finish(&s); }
    SDL_free(text);
}

int main(int argc, char **argv)
{
    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outdir = argv[++i];
            if (!SDL_CreateDirectory(outdir)) {
                fprintf(stderr, "error: can't create %s: %s\n", outdir, SDL_GetError());
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: pixelart [-o outdir] sheet.txt [more.txt ...]\n");
            return 2;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) { i++; continue; }
        process(argv[i]);
        files++;
    }
    if (files == 0) {
        fprintf(stderr, "usage: pixelart [-o outdir] sheet.txt [more.txt ...]\n");
        return 2;
    }
    printf("%d PNG%s written%s\n", written, written == 1 ? "" : "s",
           errors ? ", with errors" : "");
    return errors ? 1 : 0;
}
