/*
 * md: a Markdown viewer/editor tuned for cold start.
 *
 * Startup is arranged so that the slow parts overlap:
 *   main thread     SDL_Init -> window + renderer -> first (empty) frame -> show
 *   font thread     TTF_Init, FreeType/HarfBuzz, open Roboto + Noto Color Emoji
 *   loader thread   read file, tree-sitter parse, build block list
 *   nvim thread     write config, spawn `nvim --embed -u <config> file`
 * The main thread only blocks on the font thread right before it needs to
 * draw text, by which time the window is already on screen.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "app.h"
#include "document.h"
#include "editor.h"
#include "fonts.h"
#include "images.h"
#include "nvim.h"
#include "theme.h"
#include "view.h"

static App app;

/* ----------------------------------------------------------- workers -- */

static int SDLCALL font_thread_main(void *unused)
{
    (void)unused;
    return fonts_init(16.0f) ? 0 : -1;
}

static void wake_main_thread(void)
{
    if (app.wake_event) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = app.wake_event;
        SDL_PushEvent(&ev);
    }
}

static void publish_document(Document *doc)
{
    /* If an older result was never picked up, discard it. */
    Document *stale = SDL_SetAtomicPointer(&app.loaded_doc, doc);
    document_free(stale);
    SDL_AddAtomicInt(&app.loads_pending, -1);
    wake_main_thread();
}

static int SDLCALL loader_thread_main(void *arg)
{
    char *path = arg;
    Document *doc = document_load(path);
    SDL_free(path);
    publish_document(doc);
    return 0;
}

typedef struct ParseJob {
    char *name;
    char *text;
    size_t len;
} ParseJob;

static int SDLCALL parse_thread_main(void *arg)
{
    ParseJob *job = arg;
    Document *doc = document_parse(job->name, job->text, job->len);
    SDL_free(job->name);
    SDL_free(job);
    publish_document(doc);
    return 0;
}

static void spawn_detached(SDL_ThreadFunction fn, const char *name, void *arg)
{
    SDL_AddAtomicInt(&app.loads_pending, 1);
    SDL_Thread *t = SDL_CreateThread(fn, name, arg);
    if (t) SDL_DetachThread(t);
    else fn(arg); /* fall back to doing it inline */
}

static void start_load(const char *path)
{
    spawn_detached(loader_thread_main, "md-loader", SDL_strdup(path));
}

/* Starts parsing editor text, or queues it if a parse is running. */
static void start_preview_parse(char *name, char *text, size_t len)
{
    if (app.preview_parsing) {
        SDL_free(app.preview_pending_name);
        SDL_free(app.preview_pending_text);
        app.preview_pending_name = name;
        app.preview_pending_text = text;
        app.preview_pending_len = len;
        return;
    }
    ParseJob *job = SDL_malloc(sizeof(ParseJob));
    job->name = name;
    job->text = text;
    job->len = len;
    app.preview_parsing = true;
    spawn_detached(parse_thread_main, "md-parse", job);
}

/* Opening a file: through Neovim when it runs (the preview follows the
 * buffer), otherwise straight into the viewer. */
static void open_file(const char *path)
{
    if (app.nvim_ok && app.nvim.attached) nvim_edit_file(&app.nvim, path);
    else start_load(path);
}

static void SDLCALL open_dialog_done(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    if (!files || !files[0]) return;
    /* Called on another thread on some platforms: hand over via an event. */
    SDL_Event ev;
    SDL_zero(ev);
    char *copy = SDL_strdup(files[0]);
    ev.user.type = app.wake_event;
    ev.user.code = 1;
    ev.user.data1 = copy;
    if (!SDL_PushEvent(&ev)) SDL_free(copy);
}

static void show_open_dialog(void)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Markdown", "md;markdown;mdown;mkd;mkdn;txt"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(open_dialog_done, NULL, app.window, filters, 2, NULL, false);
}

/* ------------------------------------------------------------ layout -- */

static float display_scale(void)
{
    float s = SDL_GetWindowDisplayScale(app.window);
    return s > 0 ? s : 1.0f;
}

static bool editor_visible(void)
{
    return app.nvim_ok && app.layout != LAYOUT_VIEWER;
}

static bool viewer_visible(void)
{
    return !app.nvim_ok || app.layout != LAYOUT_EDITOR;
}

static void relayout(void)
{
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(app.renderer, &w, &h);
    float scale = display_scale();
    SDL_FRect full = {0, 0, (float)w, (float)h};
    SDL_FRect none = {0, 0, 0, 0};

    if (editor_visible() && viewer_visible()) {
        float gap = SDL_max(1.0f, SDL_floorf(scale));
        SDL_FRect first, second;
        if (app.vertical) {
            float half = SDL_floorf((float)w * 0.5f);
            first = (SDL_FRect){0, 0, half, (float)h};
            second = (SDL_FRect){half + gap, 0, (float)w - half - gap, (float)h};
        } else {
            float half = SDL_floorf((float)h * 0.5f);
            first = (SDL_FRect){0, 0, (float)w, half};
            second = (SDL_FRect){0, half + gap, (float)w, (float)h - half - gap};
        }
        app.editor_rect = app.editor_second ? second : first;
        app.viewer_rect = app.editor_second ? first : second;
    } else if (editor_visible()) {
        app.editor_rect = full;
        app.viewer_rect = none;
    } else {
        app.editor_rect = none;
        app.viewer_rect = full;
    }
    if (!editor_visible()) app.focus = PANE_VIEWER;
    else if (!viewer_visible()) app.focus = PANE_EDITOR;

    if (app.fonts_ok) {
        view_layout(&app.view, app.viewer_rect.w, app.viewer_rect.h, scale);
        if (app.nvim_ok) {
            editor_set_scale(&app.editor, scale);
            int cols, rows;
            editor_grid_size(&app.editor, app.editor_rect.w, app.editor_rect.h, &cols, &rows);
            if (editor_visible()) {
                if (!app.nvim.attached) nvim_attach(&app.nvim, cols, rows);
                else if (cols != app.nvim.cols || rows != app.nvim.rows) nvim_resize(&app.nvim, cols, rows);
            }
        }
    }
    if (app.focus == PANE_EDITOR) SDL_StartTextInput(app.window);
    else SDL_StopTextInput(app.window);
    app.dirty = true;
}

static void update_title(void)
{
    Document *doc = app.view.doc;
    const char *name = NULL;
    if (doc && doc->path && doc->path[0]) {
        name = doc->path;
        for (const char *p = doc->path; *p; ++p) {
            if (*p == '/' || *p == '\\') name = p + 1;
        }
    }
    char *title = NULL;
    if (name) SDL_asprintf(&title, "%s - " APP_NAME, name);
    SDL_SetWindowTitle(app.window, title ? title : APP_NAME);
    SDL_free(title);
}

static void take_loaded_document(void)
{
    if (!app.fonts_ok) return;
    Document *doc = SDL_SetAtomicPointer(&app.loaded_doc, NULL);
    if (!doc) return;

    bool same_file = app.view.doc && doc->path && app.view.doc->path &&
                     SDL_strcmp(doc->path, app.view.doc->path) == 0;
    view_set_document(&app.view, doc, same_file);
    update_title();
    app.dirty = true;

    /* A preview parse finished: run the one that queued up meanwhile. */
    app.preview_parsing = false;
    if (app.preview_pending_text) {
        char *name = app.preview_pending_name, *text = app.preview_pending_text;
        app.preview_pending_name = app.preview_pending_text = NULL;
        start_preview_parse(name, text, app.preview_pending_len);
    }
}

static void pump_nvim(void)
{
    Nvim *nv = &app.nvim;
    if (!app.nvim_ok) return;
    if (nvim_process(nv)) {
        if (nv->flushed) {
            nv->flushed = false;
            app.dirty = true;
        }
        if (nv->text_changed) nvim_request_text(nv);
        if (nv->buf_ready) {
            nv->buf_ready = false;
            start_preview_parse(nv->buf_name, nv->buf_text, nv->buf_text_len);
            nv->buf_name = nv->buf_text = NULL;
        }
        if (nv->scrolled) {
            nv->scrolled = false;
            if (viewer_visible()) {
                view_scroll_to_line(&app.view, nv->top_line);
                app.dirty = true;
            }
        }
    }
}

static Uint32 SDLCALL toast_expired(void *userdata, SDL_TimerID id, Uint32 interval)
{
    (void)userdata; (void)id; (void)interval;
    wake_main_thread(); /* the wake handler redraws once the toast is due */
    return 0;
}

static void show_toast(const char *msg)
{
    SDL_strlcpy(app.toast, msg, sizeof app.toast);
    app.toast_until = SDL_GetTicks() + 1500;
    SDL_AddTimer(1550, toast_expired, NULL);
    app.dirty = true;
}

/* A small pill centred at the bottom of the viewer (or the window). */
static void draw_toast(SDL_Renderer *r)
{
    if (!app.toast[0] || !app.fonts_ok) return;
    if (SDL_GetTicks() >= app.toast_until) {
        app.toast[0] = '\0';
        return;
    }
    float scale = display_scale();
    TTF_Font *font = fonts_get(FONT_MEDIUM, 14.0f * scale);
    if (!font) return;
    TTF_Text *text = TTF_CreateText(app.engine, font, app.toast, 0);
    if (!text) return;
    int tw = 0, th = 0;
    TTF_GetTextSize(text, &tw, &th);
    const SDL_FRect *area = app.viewer_rect.w > 0 ? &app.viewer_rect : &app.editor_rect;
    float pad_x = 16 * scale, pad_y = 8 * scale;
    SDL_FRect box = {area->x + SDL_floorf((area->w - tw) * 0.5f) - pad_x,
                     area->y + area->h - th - 2 * pad_y - 24 * scale,
                     tw + 2 * pad_x, th + 2 * pad_y};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (app.dark) SDL_SetRenderDrawColor(r, 0xE6, 0xE6, 0xEA, 0xF0);
    else SDL_SetRenderDrawColor(r, 0x24, 0x27, 0x2E, 0xF0);
    SDL_RenderFillRect(r, &box);
    if (app.dark) TTF_SetTextColor(text, 0x1E, 0x1E, 0x2E, 0xFF);
    else TTF_SetTextColor(text, 0xFF, 0xFF, 0xFF, 0xFF);
    TTF_DrawRendererText(text, box.x + pad_x, box.y + pad_y);
    TTF_DestroyText(text);
}

static void render(void)
{
    SDL_Renderer *r = app.renderer;
    const Theme *th = &app.view.theme;
    SDL_SetRenderViewport(r, NULL);
    SDL_SetRenderDrawColor(r, th->rule.r, th->rule.g, th->rule.b, 0xFF);
    SDL_RenderClear(r); /* the divider shows through the gap */

    if (app.fonts_ok && editor_visible() && app.nvim.cells) {
        editor_draw(&app.editor, r, &app.editor_rect, app.focus == PANE_EDITOR);
    } else if (editor_visible()) {
        SDL_SetRenderDrawColor(r, 0x1E, 0x1E, 0x2E, 0xFF); /* Mocha base until nvim draws */
        SDL_RenderFillRect(r, &app.editor_rect);
    }

    if (viewer_visible()) {
        SDL_Rect vp = {(int)app.viewer_rect.x, (int)app.viewer_rect.y,
                       (int)app.viewer_rect.w, (int)app.viewer_rect.h};
        SDL_SetRenderViewport(r, &vp);
        SDL_SetRenderDrawColor(r, th->bg.r, th->bg.g, th->bg.b, 0xFF);
        SDL_FRect bg = {0, 0, app.viewer_rect.w, app.viewer_rect.h};
        SDL_RenderFillRect(r, &bg);
        if (app.fonts_ok) {
            Document *doc = app.view.doc;
            if (doc && doc->error) {
                view_draw_message(&app.view, r, "Could not open the file", doc->error);
            } else if (doc && doc->block_count > 0) {
                view_draw(&app.view, r);
            } else if (!editor_visible() && SDL_GetAtomicInt(&app.loads_pending) == 0) {
                view_draw_message(&app.view, r, "Drop a Markdown file here",
                                  app.nvim_ok || !app.nvim.error ? "or press Ctrl+O to open one"
                                                                 : app.nvim.error);
            }
        }
        SDL_SetRenderViewport(r, NULL);
    }
    draw_toast(r);
    SDL_RenderPresent(r);
    app.dirty = false;
}

/* ------------------------------------------------------------- input -- */

static void request_quit(void)
{
    if (app.nvim_ok && app.nvim.attached && !nvim_has_exited(&app.nvim)) {
        /* Let Neovim decide (it refuses with unsaved changes); md exits when
         * nvim does. */
        if (app.layout == LAYOUT_VIEWER) {
            app.layout = LAYOUT_SPLIT;
            app.focus = PANE_EDITOR;
            relayout();
        }
        nvim_request_quit(&app.nvim);
    } else {
        app.quitting = true;
    }
}

static const char *special_key_name(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "CR";
    case SDLK_ESCAPE:    return "Esc";
    case SDLK_BACKSPACE: return "BS";
    case SDLK_TAB:       return "Tab";
    case SDLK_DELETE:    return "Del";
    case SDLK_INSERT:    return "Insert";
    case SDLK_UP:        return "Up";
    case SDLK_DOWN:      return "Down";
    case SDLK_LEFT:      return "Left";
    case SDLK_RIGHT:     return "Right";
    case SDLK_HOME:      return "Home";
    case SDLK_END:       return "End";
    case SDLK_PAGEUP:    return "PageUp";
    case SDLK_PAGEDOWN:  return "PageDown";
    case SDLK_F1: return "F1";   case SDLK_F2: return "F2";   case SDLK_F3: return "F3";
    case SDLK_F4: return "F4";   case SDLK_F5: return "F5";   case SDLK_F6: return "F6";
    case SDLK_F7: return "F7";   case SDLK_F8: return "F8";   case SDLK_F9: return "F9";
    case SDLK_F10: return "F10"; case SDLK_F11: return "F11"; case SDLK_F12: return "F12";
    default: return NULL;
    }
}

/* Key presses that do not produce SDL text input: special keys and
 * Ctrl/Alt chords. Plain text (including AltGr characters such as Polish
 * ą ś ó) arrives through SDL_EVENT_TEXT_INPUT instead. */
static void editor_key(const SDL_KeyboardEvent *k)
{
    const bool altgr = (k->mod & SDL_KMOD_RALT) != 0;
    const bool ctrl = (k->mod & SDL_KMOD_CTRL) != 0 && !altgr;
    const bool alt = (k->mod & SDL_KMOD_LALT) != 0;
    const bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;

    char mods[16] = "";
    if (ctrl) SDL_strlcat(mods, "C-", sizeof(mods));
    if (alt) SDL_strlcat(mods, "A-", sizeof(mods));

    char keys[48];
    const char *name = special_key_name(k->key);
    if (name) {
        SDL_snprintf(keys, sizeof(keys), "<%s%s%s>", shift ? "S-" : "", mods, name);
        nvim_input(&app.nvim, keys);
        return;
    }
    if (!(ctrl || alt)) return; /* text input handles it */

    SDL_Keycode key = k->key;
    if (key == SDLK_SPACE) {
        SDL_snprintf(keys, sizeof(keys), "<%sSpace>", mods);
    } else if (key >= 0x21 && key < 0x7f) {
        char ch = (char)key;
        if (shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        if (ch == '<') SDL_snprintf(keys, sizeof(keys), "<%slt>", mods);
        else if (ch == '\\') SDL_snprintf(keys, sizeof(keys), "<%sBslash>", mods);
        else SDL_snprintf(keys, sizeof(keys), "<%s%c>", mods, ch);
    } else {
        return;
    }
    nvim_input(&app.nvim, keys);
}

static void editor_text(const char *text)
{
    /* nvim_input treats "<" as the start of a key name. */
    char buf[256];
    size_t n = 0;
    for (const char *p = text; *p && n + 5 < sizeof(buf); ++p) {
        if (*p == '<') { SDL_memcpy(buf + n, "<lt>", 4); n += 4; }
        else buf[n++] = *p;
    }
    buf[n] = 0;
    nvim_input(&app.nvim, buf);
}

static void viewer_key(const SDL_KeyboardEvent *k)
{
    View *v = &app.view;
    const float line = view_line_height(v);
    const float page = SDL_max(v->height - 2 * line, line);
    switch (k->key) {
    case SDLK_UP:       view_scroll_by(v, -line); break;
    case SDLK_DOWN:     view_scroll_by(v, line); break;
    case SDLK_PAGEUP:   view_scroll_by(v, -page); break;
    case SDLK_PAGEDOWN: view_scroll_by(v, page); break;
    case SDLK_SPACE:    view_scroll_by(v, (k->mod & SDL_KMOD_SHIFT) ? -page : page); break;
    case SDLK_HOME:     view_scroll_to(v, 0); break;
    case SDLK_END:      view_scroll_to_end(v); break;
    default:            return;
    }
    app.dirty = true;
}

static void set_layout(Layout layout)
{
    if (!app.nvim_ok) return;
    app.layout = app.layout == layout ? LAYOUT_SPLIT : layout;
    if (app.layout == LAYOUT_SPLIT) app.focus = PANE_EDITOR;
    relayout();
}

static void copy_selection(void)
{
    char *text = view_selection_text(&app.view);
    if (!text || !text[0]) {
        SDL_free(text);
        show_toast("Nothing selected");
        return;
    }
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xC0) != 0x80) ++chars;
    }
    char msg[96];
    if (SDL_SetClipboardText(text)) {
        SDL_snprintf(msg, sizeof msg, "Copied %zu character%s", chars, chars == 1 ? "" : "s");
    } else {
        SDL_snprintf(msg, sizeof msg, "Could not copy: %s", SDL_GetError());
    }
    show_toast(msg);
    SDL_free(text);
}

/* App shortcuts use Ctrl+Shift so they never collide with AltGr letters
 * (AltGr = Ctrl+Alt on Windows) or with Neovim's own Ctrl mappings. */
static bool app_shortcut(const SDL_KeyboardEvent *k)
{
    const bool ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (k->mod & SDL_KMOD_ALT) != 0;
    if (!ctrl || !shift || alt) {
        if (!ctrl || shift || alt || app.focus != PANE_VIEWER) return false;
        switch (k->key) {
        case SDLK_O:
            show_open_dialog();
            return true;
        case SDLK_A:
            view_select_all(&app.view);
            app.dirty = true;
            return true;
        case SDLK_C:
        case SDLK_INSERT:
            copy_selection();
            return true;
        default:
            return false;
        }
    }
    switch (k->key) {
    case SDLK_X: /* swap which side the editor is on */
        app.editor_second = !app.editor_second;
        relayout();
        return true;
    case SDLK_O: /* split orientation */
        app.vertical = !app.vertical;
        relayout();
        return true;
    case SDLK_S: /* split on/off (off = viewer only) */
        set_layout(LAYOUT_VIEWER);
        return true;
    case SDLK_E: /* editor only on/off */
        set_layout(LAYOUT_EDITOR);
        return true;
    case SDLK_TAB: /* move keyboard focus between panes */
        if (editor_visible() && viewer_visible()) {
            app.focus = app.focus == PANE_EDITOR ? PANE_VIEWER : PANE_EDITOR;
            relayout();
        }
        return true;
    case SDLK_N: /* new file / open dialog */
        show_open_dialog();
        return true;
    default:
        return false;
    }
}

static bool in_rect(const SDL_FRect *r, float x, float y)
{
    return r->w > 0 && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static void to_pixels(float *x, float *y)
{
    /* Mouse coordinates are in window points; layout is in pixels. */
    float s = SDL_GetWindowPixelDensity(app.window);
    *x *= s;
    *y *= s;
}

static const char *mouse_mods(void)
{
    SDL_Keymod m = SDL_GetModState();
    static char buf[8];
    buf[0] = 0;
    if (m & SDL_KMOD_CTRL) SDL_strlcat(buf, "C", sizeof(buf));
    if (m & SDL_KMOD_SHIFT) SDL_strlcat(buf, "S", sizeof(buf));
    if (m & SDL_KMOD_ALT) SDL_strlcat(buf, "A", sizeof(buf));
    return buf;
}

static void editor_mouse(const char *button, const char *action, float x, float y)
{
    int row, col;
    editor_cell_at(&app.editor, x - app.editor_rect.x, y - app.editor_rect.y, &row, &col);
    nvim_input_mouse(&app.nvim, button, action, mouse_mods(), row, col);
}

static void set_hand_cursor(bool hand)
{
    static SDL_Cursor *pointer, *arrow;
    static bool current;
    if (hand == current) return;
    if (!pointer) {
        pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    }
    SDL_SetCursor(hand ? pointer : arrow);
    current = hand;
}

/* Web links go to the browser; links to local Markdown files open in md
 * (in the editor when it runs); other local files use the OS handler. */
static void open_link(const char *url, size_t len)
{
    char *u = SDL_strndup(url, len);
    if (u[0] == '#') { SDL_free(u); return; } /* in-page anchors: not yet */
    if (SDL_strstr(u, "://") || SDL_strncasecmp(u, "mailto:", 7) == 0) {
        SDL_OpenURL(u);
        SDL_free(u);
        return;
    }
    char *hash = SDL_strchr(u, '#');
    if (hash) *hash = 0;
    const char *doc_path = app.view.doc ? app.view.doc->path : NULL;
    char *path = NULL;
    bool absolute = u[0] == '/' || u[0] == '\\' || (u[0] && u[1] == ':');
    const char *slash = NULL;
    if (doc_path) for (const char *q = doc_path; *q; ++q) if (*q == '/' || *q == '\\') slash = q;
    if (absolute || !slash) path = SDL_strdup(u);
    else SDL_asprintf(&path, "%.*s/%s", (int)(slash - doc_path), doc_path, u);
    size_t n = SDL_strlen(path);
    bool markdown = (n > 3 && SDL_strcasecmp(path + n - 3, ".md") == 0) ||
                    (n > 9 && SDL_strcasecmp(path + n - 9, ".markdown") == 0);
    if (markdown) {
        open_file(path);
    } else {
        char *file_url = NULL;
        SDL_asprintf(&file_url, "file:///%s", path[0] == '/' ? path + 1 : path);
        SDL_OpenURL(file_url);
        SDL_free(file_url);
    }
    SDL_free(path);
    SDL_free(u);
}

/* Returns false when the app should quit. */
static bool handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
        request_quit();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (app_shortcut(&e->key)) break;
        if ((e->key.mod & SDL_KMOD_CTRL) && !(e->key.mod & SDL_KMOD_RALT) &&
            (e->key.key == SDLK_Q) && app.focus == PANE_VIEWER) {
            request_quit();
            break;
        }
        if (app.focus == PANE_EDITOR && app.nvim_ok) editor_key(&e->key);
        else viewer_key(&e->key);
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (app.focus == PANE_EDITOR && app.nvim_ok) editor_text(e->text.text);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float x = e->wheel.mouse_x, y = e->wheel.mouse_y;
        to_pixels(&x, &y);
        if (editor_visible() && in_rect(&app.editor_rect, x, y)) {
            const char *dir = e->wheel.y > 0 ? "up" : "down";
            int steps = (int)SDL_ceilf(SDL_fabsf(e->wheel.y));
            for (int i = 0; i < steps && i < 10; ++i) editor_mouse("wheel", dir, x, y);
        } else {
            view_scroll_by(&app.view, -e->wheel.y * 3 * view_line_height(&app.view));
            app.dirty = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        float x = e->button.x, y = e->button.y;
        to_pixels(&x, &y);
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        float vx = x - app.viewer_rect.x, vy = y - app.viewer_rect.y;
        if (e->button.button == SDL_BUTTON_LEFT && !down && app.selecting) {
            /* A click (no drag) on a link opens it; otherwise it just
             * clears the selection. */
            app.selecting = false;
            if (!app.sel_dragged) {
                const char *url;
                size_t len;
                view_select_clear(&app.view);
                app.dirty = true;
                if (in_rect(&app.viewer_rect, x, y) && view_link_at(&app.view, vx, vy, &url, &len)) {
                    open_link(url, len);
                }
            }
            break;
        }
        if (down && e->button.button == SDL_BUTTON_LEFT && viewer_visible() &&
            in_rect(&app.viewer_rect, x, y)) {
            if ((SDL_GetModState() & SDL_KMOD_SHIFT) && app.view.has_sel) {
                view_select_extend(&app.view, vx, vy);
                app.sel_dragged = true;
            } else {
                view_select_start(&app.view, vx, vy);
                app.sel_dragged = false;
            }
            app.selecting = true;
            app.press_x = x;
            app.press_y = y;
            app.dirty = true;
        }
        if (down) {
            Pane target = editor_visible() && in_rect(&app.editor_rect, x, y) ? PANE_EDITOR : PANE_VIEWER;
            if (target != app.focus) {
                app.focus = target;
                relayout();
            }
            app.mouse_down_editor = target == PANE_EDITOR;
        }
        if (app.mouse_down_editor && e->button.button <= SDL_BUTTON_RIGHT) {
            const char *btn = e->button.button == SDL_BUTTON_LEFT ? "left"
                            : e->button.button == SDL_BUTTON_RIGHT ? "right" : "middle";
            editor_mouse(btn, down ? "press" : "release", x, y);
        }
        if (!down) app.mouse_down_editor = false;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (viewer_visible()) {
            float x = e->motion.x, y = e->motion.y;
            to_pixels(&x, &y);
            const char *url;
            size_t len;
            bool link = in_rect(&app.viewer_rect, x, y) &&
                        view_link_at(&app.view, x - app.viewer_rect.x, y - app.viewer_rect.y, &url, &len);
            set_hand_cursor(link);
        }
        if (app.selecting && (e->motion.state & SDL_BUTTON_LMASK)) {
            float x = e->motion.x, y = e->motion.y;
            to_pixels(&x, &y);
            float slop = 4 * display_scale();
            if (!app.sel_dragged &&
                (SDL_fabsf(x - app.press_x) > slop || SDL_fabsf(y - app.press_y) > slop)) {
                app.sel_dragged = true;
            }
            if (app.sel_dragged) {
                /* Dragging past the top/bottom edge scrolls. */
                const SDL_FRect *vr = &app.viewer_rect;
                if (y < vr->y) view_scroll_by(&app.view, y - vr->y);
                else if (y > vr->y + vr->h) view_scroll_by(&app.view, y - (vr->y + vr->h));
                view_select_extend(&app.view, x - vr->x, y - vr->y);
                app.dirty = true;
            }
        }
        if (app.mouse_down_editor && (e->motion.state & SDL_BUTTON_LMASK)) {
            float x = e->motion.x, y = e->motion.y;
            to_pixels(&x, &y);
            editor_mouse("left", "drag", x, y);
        }
        break;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) open_file(e->drop.data);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        relayout();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        app.dirty = true;
        break;
    case SDL_EVENT_SYSTEM_THEME_CHANGED: {
        bool dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
        if (dark != app.dark && app.fonts_ok) {
            app.dark = dark;
            /* Colours are baked into texts at creation; rebuild them. */
            Document *doc = app.view.doc;
            app.view.doc = NULL;
            app.view.theme = dark ? THEME_DARK : THEME_LIGHT;
            view_set_document(&app.view, doc, true);
            app.dirty = true;
        }
        break;
    }
    default:
        if (e->type == app.wake_event) {
            if (e->user.code == 1 && e->user.data1) { /* file dialog result */
                open_file(e->user.data1);
                SDL_free(e->user.data1);
            }
            if (app.toast[0] && SDL_GetTicks() >= app.toast_until) app.dirty = true;
            take_loaded_document();
            pump_nvim();
            if (app.fonts_ok && images_pump(app.renderer)) {
                view_relayout(&app.view);
                app.dirty = true;
            }
            if (app.nvim_ok && nvim_has_exited(&app.nvim)) return false;
        }
        break;
    }
    return !app.quitting;
}

/* -------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    const char *file = argc > 1 ? argv[1] : NULL;

    /* 1. Kick off background work before touching the window system. */
    app.font_thread = SDL_CreateThread(font_thread_main, "md-fonts", NULL);
    if (file) start_load(file);
    nvim_start_async(&app.nvim, file, 0);

    /* 2. Window + renderer. */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
        return 1;
    }
    app.wake_event = SDL_RegisterEvents(1);
    images_init(app.wake_event);
    app.dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    /* Open as a viewer; Ctrl+Shift+S brings in the editor. Neovim still
     * starts in the background so that is instant. */
    app.layout = LAYOUT_VIEWER;
    app.vertical = true;
    app.focus = PANE_VIEWER;

    if (!SDL_CreateWindowAndRenderer(APP_NAME, 1280, 820,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                     SDL_WINDOW_HIDDEN,
                                     &app.window, &app.renderer)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
        return 1;
    }

    /* 3. First frame: just the background colours, then show the window.
     *    Presenting before showing avoids a white/black flash. */
    const Theme *theme = app.dark ? &THEME_DARK : &THEME_LIGHT;
    app.view.theme = *theme;
    app.nvim_ok = true; /* assume nvim works until we know it failed */
    relayout();
    render();
    SDL_ShowWindow(app.window);

    /* 4. Text needs the fonts; by now they are usually ready. */
    int font_status = -1;
    SDL_WaitThread(app.font_thread, &font_status);
    app.font_thread = NULL;
    app.fonts_ok = font_status == 0;
    if (!app.fonts_ok) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME,
                                 "Could not load the embedded fonts.", app.window);
    } else {
        app.engine = TTF_CreateRendererTextEngine(app.renderer);
    }
    view_init(&app.view, app.engine, theme);

    /* 5. Neovim: process creation overlapped with everything above. */
    app.nvim.wake_event = app.wake_event;
    app.nvim_ok = app.fonts_ok && nvim_wait_started(&app.nvim);
    if (app.nvim_ok) editor_init(&app.editor, app.engine, &app.nvim);
    else app.layout = LAYOUT_VIEWER;

    relayout(); /* also attaches the UI */
    take_loaded_document(); /* in case the loader already finished */
    render();

    /* 6. Event loop: sleep until something happens. */
    bool running = true;
    while (running) {
        SDL_Event e;
        if (app.fonts_ok && view_layout_pending(&app.view)) {
            /* Lay out the rest of a long document while idle, in small
             * slices so input stays responsive. */
            if (!SDL_PollEvent(&e)) {
                view_layout_more(&app.view, 4);
                continue;
            }
        } else if (!SDL_WaitEvent(&e)) {
            break;
        }
        do {
            running = handle_event(&e);
        } while (running && SDL_PollEvent(&e));
        if (running && app.dirty) render();
    }

    if (app.nvim_ok) editor_destroy(&app.editor);
    nvim_stop(&app.nvim);
    view_destroy(&app.view);
    document_free(SDL_SetAtomicPointer(&app.loaded_doc, NULL));
    SDL_free(app.preview_pending_name);
    SDL_free(app.preview_pending_text);
    images_shutdown();
    if (app.engine) TTF_DestroyRendererTextEngine(app.engine);
    if (app.fonts_ok) fonts_shutdown();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
