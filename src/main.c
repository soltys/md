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
        /* A new size re-wraps both panes differently: line the preview up
         * with the editor again rather than keeping its old pixel offset. */
        if (editor_visible() && viewer_visible() && app.nvim.attached && app.nvim.top_line > 0) {
            view_scroll_to_line(&app.view, app.nvim.top_line);
        }
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
    /* On for both panes: the preview reads "/" and its search text as text,
     * whatever the keyboard layout needs to type them. */
    SDL_StartTextInput(app.window);
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
    /* A preview of the editor's buffer follows the editor, whatever the
     * path spelling (relative on the command line, absolute from nvim). */
    bool follow = editor_visible() && app.nvim.attached && app.nvim.top_line > 0;
    view_set_document(&app.view, doc, same_file || follow);
    if (follow) view_scroll_to_line(&app.view, app.nvim.top_line);
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
            /* Skip the echo of a scroll md itself asked for. */
            bool echo = nv->top_line == app.sent_top ||
                        SDL_GetTicks() - app.sent_at < 250;
            if (viewer_visible() && !echo) {
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

static void show_toast_ms(const char *msg, Uint32 ms)
{
    SDL_strlcpy(app.toast, msg, sizeof app.toast);
    app.toast_until = SDL_GetTicks() + ms;
    SDL_AddTimer(ms + 50, toast_expired, NULL);
    app.dirty = true;
}

static void show_toast(const char *msg) { show_toast_ms(msg, 1500); }

/* A small pill centred at the bottom of the viewer (or the window). */
/* "/query" along the bottom of the preview while typing a search. */
static void draw_search_bar(SDL_Renderer *r)
{
    if (!app.searching || !app.fonts_ok || !viewer_visible()) return;
    const float s = display_scale();
    const Theme *th = &app.view.theme;
    TTF_Font *font = fonts_get(FONT_MONO, 14.0f * s);
    if (!font) return;
    const float h = SDL_floorf(30.0f * s);
    const SDL_FRect *vr = &app.viewer_rect;
    SDL_FRect bar = {vr->x, vr->y + vr->h - h, vr->w, h};
    SDL_SetRenderDrawColor(r, th->code_bg.r, th->code_bg.g, th->code_bg.b, 0xFF);
    SDL_RenderFillRect(r, &bar);
    SDL_SetRenderDrawColor(r, th->rule.r, th->rule.g, th->rule.b, 0xFF);
    SDL_FRect line = {bar.x, bar.y, bar.w, SDL_max(1.0f, SDL_floorf(s))};
    SDL_RenderFillRect(r, &line);

    char buf[300];
    SDL_snprintf(buf, sizeof buf, "%c%s", app.search_back ? '?' : '/', app.search);
    TTF_Text *t = TTF_CreateText(app.engine, font, buf, 0);
    if (!t) return;
    int tw = 0, tht = 0;
    TTF_GetTextSize(t, &tw, &tht);
    const float x = bar.x + 12.0f * s, y = bar.y + SDL_floorf((h - (float)tht) * 0.5f);
    TTF_SetTextColor(t, th->text.r, th->text.g, th->text.b, 0xFF);
    TTF_DrawRendererText(t, x, y);
    TTF_DestroyText(t);
    SDL_FRect caret = {x + (float)tw + 1.0f, y, SDL_max(2.0f, SDL_floorf(2.0f * s)), (float)tht};
    SDL_SetRenderDrawColor(r, th->accent.r, th->accent.g, th->accent.b, 0xFF);
    SDL_RenderFillRect(r, &caret);
    if (app.search[0] && !app.search_found) {
        TTF_Text *m = TTF_CreateText(app.engine, font, "no match", 0);
        if (m) {
            int mw = 0, mh = 0;
            TTF_GetTextSize(m, &mw, &mh);
            TTF_SetTextColor(m, th->muted.r, th->muted.g, th->muted.b, 0xFF);
            TTF_DrawRendererText(m, bar.x + bar.w - (float)mw - 12.0f * s, y);
            TTF_DestroyText(m);
        }
    }
}

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
    const float w = (float)tw, h = (float)th;
    SDL_FRect box = {area->x + SDL_floorf((area->w - w) * 0.5f) - pad_x,
                     area->y + area->h - h - 2 * pad_y - 24 * scale,
                     w + 2 * pad_x, h + 2 * pad_y};
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    if (app.dark) SDL_SetRenderDrawColor(r, 0xE6, 0xE6, 0xEA, 0xF0);
    else SDL_SetRenderDrawColor(r, 0x24, 0x27, 0x2E, 0xF0);
    SDL_RenderFillRect(r, &box);
    if (app.dark) TTF_SetTextColor(text, 0x1E, 0x1E, 0x2E, 0xFF);
    else TTF_SetTextColor(text, 0xFF, 0xFF, 0xFF, 0xFF);
    TTF_DrawRendererText(text, box.x + pad_x, box.y + pad_y);
    TTF_DestroyText(text);
}

/* F1: every shortcut, in a panel over the whole window. */
static const char *const HELP[][2] = {
    {"F1", "Show / hide this help"},
    {NULL, "Layout"},
    {"Ctrl+Shift+S", "Editor beside the preview on / off"},
    {"Ctrl+Shift+E", "Editor only on / off"},
    {"Ctrl+Shift+O", "Side by side / stacked"},
    {"Ctrl+Shift+X", "Swap which side the editor is on"},
    {"Ctrl+Shift+Tab", "Move focus between editor and preview"},
    {NULL, "Files"},
    {"Ctrl+Shift+N", "Open a file (or drop one on the window)"},
    {"Ctrl+O", "Open a file (preview focused)"},
    {"Ctrl+Q", "Quit (preview focused)"},
    {"md.exe -e file.md", "Start with the editor open"},
    {NULL, "Preview"},
    {"Wheel, PgUp / PgDn", "Scroll (the editor follows)"},
    {"Space, Home / End", "Scroll a page, to the start / end"},
    {"j / k", "Scroll a line down / up (5j: five lines)"},
    {"Ctrl+d / Ctrl+u", "Half a page down / up"},
    {"Ctrl+f / Ctrl+b", "A page down / up"},
    {"gg / G", "Start / end (42G: source line 42)"},
    {"/ or ?", "Search forward / backward (Enter, Esc)"},
    {"n / N", "Next / previous match"},
    {"Drag, Shift+click", "Select text"},
    {"Ctrl+A", "Select all"},
    {"Ctrl+C", "Copy the selection"},
    {"Click a link", "Open it (.md files open in md)"},
};

static void draw_help(SDL_Renderer *r)
{
    if (!app.show_help || !app.fonts_ok) return;
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(r, &w, &h);
    const float s = display_scale();
    const Theme *th = &app.view.theme;
    TTF_Font *title_font = fonts_get(FONT_MEDIUM, 22.0f * s);
    TTF_Font *head_font = fonts_get(FONT_MEDIUM, 13.0f * s);
    TTF_Font *key_font = fonts_get(FONT_MONO, 14.0f * s);
    TTF_Font *text_font = fonts_get(FONT_REGULAR, 15.0f * s);
    if (!title_font || !head_font || !key_font || !text_font) return;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0x8C);
    SDL_FRect all = {0, 0, (float)w, (float)h};
    SDL_RenderFillRect(r, &all);

    const int n = (int)SDL_arraysize(HELP);
    const float row = 26.0f * s, head = 34.0f * s, pad = 28.0f * s;
    const float key_w = 190.0f * s;
    float body_h = 0;
    for (int i = 0; i < n; ++i) body_h += HELP[i][0] ? row : head;
    const float pw = SDL_min(620.0f * s, (float)w - 32.0f * s);
    const float ph = SDL_min(pad * 2 + 40.0f * s + body_h, (float)h - 32.0f * s);
    SDL_FRect panel = {SDL_floorf(((float)w - pw) * 0.5f), SDL_floorf(((float)h - ph) * 0.5f), pw, ph};
    SDL_SetRenderDrawColor(r, th->bg.r, th->bg.g, th->bg.b, 0xFF);
    SDL_RenderFillRect(r, &panel);
    SDL_SetRenderDrawColor(r, th->rule.r, th->rule.g, th->rule.b, 0xFF);
    SDL_RenderRect(r, &panel);

    SDL_Rect clip = {(int)panel.x, (int)panel.y, (int)panel.w, (int)panel.h};
    SDL_SetRenderClipRect(r, &clip);
    float x = panel.x + pad, y = panel.y + pad;
    TTF_Text *t = TTF_CreateText(app.engine, title_font, "Keyboard shortcuts", 0);
    if (t) {
        TTF_SetTextColor(t, th->heading.r, th->heading.g, th->heading.b, 0xFF);
        TTF_DrawRendererText(t, x, y);
        TTF_DestroyText(t);
    }
    y += 40.0f * s;
    for (int i = 0; i < n; ++i) {
        if (!HELP[i][0]) { /* section heading */
            t = TTF_CreateText(app.engine, head_font, HELP[i][1], 0);
            if (t) {
                TTF_SetTextColor(t, th->muted.r, th->muted.g, th->muted.b, 0xFF);
                TTF_DrawRendererText(t, x, y + head - row);
                TTF_DestroyText(t);
            }
            y += head;
            continue;
        }
        t = TTF_CreateText(app.engine, key_font, HELP[i][0], 0);
        if (t) {
            TTF_SetTextColor(t, th->accent.r, th->accent.g, th->accent.b, 0xFF);
            TTF_DrawRendererText(t, x, y + 2.0f * s);
            TTF_DestroyText(t);
        }
        t = TTF_CreateText(app.engine, text_font, HELP[i][1], 0);
        if (t) {
            TTF_SetTextColor(t, th->text.r, th->text.g, th->text.b, 0xFF);
            TTF_DrawRendererText(t, x + key_w, y);
            TTF_DestroyText(t);
        }
        y += row;
    }
    SDL_SetRenderClipRect(r, NULL);
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
    draw_search_bar(r);
    draw_help(r);
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

/* ------------------------------------------------------------ search -- */

static void search_begin(bool backward)
{
    if (!app.view.doc) return;
    app.searching = true;
    app.search_back = backward;
    app.search[0] = '\0';
    app.search_found = false;
    app.search_origin = view_top_pos(&app.view);
    app.search_scroll0 = app.view.scroll;
    app.dirty = true;
}

/* Incremental: every change searches again from where the search began. */
static void search_update(void)
{
    if (app.search[0] &&
        view_search(&app.view, app.search, app.search_origin, !app.search_back, NULL)) {
        app.search_found = true;
    } else {
        app.search_found = false;
        view_select_clear(&app.view);
        view_scroll_to(&app.view, app.search_scroll0);
    }
    app.viewer_scrolled = true;
    app.dirty = true;
}

static void search_end(bool keep)
{
    app.searching = false;
    if (keep && app.search[0]) {
        SDL_strlcpy(app.last_search, app.search, sizeof app.last_search);
        app.last_search_back = app.search_back;
        if (!app.search_found) {
            char msg[300];
            SDL_snprintf(msg, sizeof msg, "Not found: %s", app.search);
            show_toast(msg);
        }
    } else {
        view_select_clear(&app.view);
        view_scroll_to(&app.view, app.search_scroll0);
        app.viewer_scrolled = true;
    }
    app.dirty = true;
}

/* n / N: the next match in the search's direction, or against it. */
static void search_next(bool reverse)
{
    if (!app.last_search[0]) return;
    const bool forward = app.last_search_back == reverse;
    SelPos from;
    if (view_has_selection(&app.view)) {
        from = view_selection_start(&app.view);
        if (forward) from.off += 1; /* past the current match */
    } else {
        from = view_top_pos(&app.view);
    }
    bool wrapped = false;
    if (view_search(&app.view, app.last_search, from, forward, &wrapped)) {
        if (wrapped) show_toast(forward ? "Search hit bottom, continuing at top"
                                        : "Search hit top, continuing at bottom");
    } else {
        char msg[300];
        SDL_snprintf(msg, sizeof msg, "Not found: %s", app.last_search);
        show_toast(msg);
    }
    app.viewer_scrolled = true;
    app.dirty = true;
}

static void search_key(const SDL_KeyboardEvent *k)
{
    switch (k->key) {
    case SDLK_ESCAPE:
        search_end(false);
        break;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        search_end(true);
        break;
    case SDLK_BACKSPACE: {
        size_t n = SDL_strlen(app.search);
        if (n == 0) { search_end(false); break; }
        while (n > 0 && ((unsigned char)app.search[n - 1] & 0xC0) == 0x80) --n; /* UTF-8 */
        app.search[n > 0 ? n - 1 : 0] = '\0';
        search_update();
        break;
    }
    default:
        break; /* the text arrives as SDL_EVENT_TEXT_INPUT */
    }
}

static void search_text(const char *text)
{
    SDL_strlcat(app.search, text, sizeof app.search);
    search_update();
}

/* Vim-style keys for the preview: [count]j/k, gg, [count]G, Ctrl+d/u
 * (half page), Ctrl+f/b (page). Counts are typed as digits first. */
static void viewer_key(const SDL_KeyboardEvent *k)
{
    View *v = &app.view;
    const float line = view_line_height(v);
    const float page = SDL_max(v->height - 2 * line, line);
    const bool ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (k->mod & SDL_KMOD_ALT) != 0;
    static int count;          /* pending [count] */
    static Uint64 g_pressed;   /* time of a first 'g', for "gg" */

    if (!ctrl && !alt && !shift && k->key >= SDLK_0 && k->key <= SDLK_9 &&
        (count > 0 || k->key != SDLK_0)) {
        count = SDL_min(count * 10 + (int)(k->key - SDLK_0), 1000000);
        return;
    }
    const int n = count > 0 ? count : 1;
    const bool had_count = count > 0;
    count = 0;
    if (k->key != SDLK_G || shift || ctrl) g_pressed = 0;

    switch (k->key) {
    case SDLK_UP:       view_scroll_by(v, -line); break;
    case SDLK_DOWN:     view_scroll_by(v, line); break;
    case SDLK_PAGEUP:   view_scroll_by(v, -page); break;
    case SDLK_PAGEDOWN: view_scroll_by(v, page); break;
    case SDLK_SPACE:    view_scroll_by(v, shift ? -page : page); break;
    case SDLK_HOME:     view_scroll_to(v, 0); break;
    case SDLK_END:      view_scroll_to_end(v); break;
    case SDLK_N:
        if (ctrl || alt) return;
        search_next(shift);
        return;
    case SDLK_J:
        if (ctrl || alt) return;
        view_scroll_by(v, (float)n * line);
        break;
    case SDLK_K:
        if (ctrl || alt) return;
        view_scroll_by(v, -(float)n * line);
        break;
    case SDLK_D: case SDLK_U:
        if (!ctrl || shift || alt) return;
        view_scroll_by(v, (k->key == SDLK_D ? 1.0f : -1.0f) * (float)n * SDL_floorf(v->height * 0.5f));
        break;
    case SDLK_F: case SDLK_B:
        if (!ctrl || shift || alt) return;
        view_scroll_by(v, (k->key == SDLK_F ? 1.0f : -1.0f) * (float)n * page);
        break;
    case SDLK_G:
        if (ctrl || alt) return;
        if (shift) { /* G: end, or [count]G: that source line */
            if (had_count) view_scroll_to_line(v, n);
            else view_scroll_to_end(v);
        } else if (g_pressed && SDL_GetTicks() - g_pressed < 1000) {
            g_pressed = 0; /* gg: start, or [count]gg: that source line */
            if (had_count) view_scroll_to_line(v, n);
            else view_scroll_to(v, 0);
        } else {
            g_pressed = SDL_GetTicks();
            if (had_count) count = n; /* keep it for the second g */
            return;
        }
        break;
    default:            return;
    }
    app.viewer_scrolled = true;
    app.dirty = true;
}

/* Neovim starts only when the editor is first asked for. Spawning the
 * process is quick; nvim's own startup runs while md keeps going, and the
 * pane fills in once it draws. */
static void begin_nvim(const char *file)
{
    app.nvim_started = true;
    app.nvim_starting = true;
    nvim_start_async(&app.nvim, file, app.wake_event);
}

static bool ensure_nvim(void)
{
    if (!app.nvim_started) {
        if (!app.fonts_ok) return false;
        begin_nvim(app.view.doc && app.view.doc->path && app.view.doc->path[0]
                   ? app.view.doc->path : app.start_file);
    }
    if (!app.nvim_starting) return app.nvim_ok;
    app.nvim_starting = false;
    app.nvim.wake_event = app.wake_event;
    app.nvim_ok = app.fonts_ok && nvim_wait_started(&app.nvim);
    if (app.nvim_ok) {
        editor_init(&app.editor, app.engine, &app.nvim);
    } else {
        char msg[96];
        SDL_snprintf(msg, sizeof msg, "Could not start Neovim%s%s",
                     app.nvim.error ? ": " : "", app.nvim.error ? app.nvim.error : "");
        show_toast_ms(msg, 4000);
    }
    return app.nvim_ok;
}

static void set_layout(Layout layout)
{
    if (!ensure_nvim()) return;
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
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In place: "My%20Notes.md" -> "My Notes.md". */
static void percent_decode(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ++r) {
        int hi, lo;
        if (r[0] == '%' && (hi = hex_digit(r[1])) >= 0 && (lo = hex_digit(r[2])) >= 0) {
            *w++ = (char)(hi * 16 + lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static bool is_markdown_path(const char *path)
{
    static const char *const exts[] = {".md", ".markdown", ".mdown", ".mkd", ".mkdn"};
    size_t n = SDL_strlen(path);
    for (size_t i = 0; i < SDL_arraysize(exts); ++i) {
        size_t e = SDL_strlen(exts[i]);
        if (n > e && SDL_strcasecmp(path + n - e, exts[i]) == 0) return true;
    }
    return false;
}

static void open_link(const char *url, size_t len)
{
    char *u = SDL_strndup(url, len);
    if (u[0] == '#') { SDL_free(u); return; } /* in-page anchors: not yet */
    char *local = u;
    if (SDL_strncasecmp(u, "file://", 7) == 0) {
        /* file:///C:/x.md and file:///home/x.md: local files like any other. */
        local = u + 7;
        if (local[0] == '/' && local[1] && local[2] == ':') ++local;
    } else if (SDL_strstr(u, "://") || SDL_strncasecmp(u, "mailto:", 7) == 0) {
        SDL_OpenURL(u);
        SDL_free(u);
        return;
    }
    char *query = SDL_strpbrk(local, "#?");
    if (query) *query = 0;
    percent_decode(local);
    if (!local[0]) { SDL_free(u); return; }

    const char *doc_path = app.view.doc ? app.view.doc->path : NULL;
    if ((!doc_path || !doc_path[0]) && app.start_file) doc_path = app.start_file;
    char *path = NULL;
    bool absolute = local[0] == '/' || local[0] == '\\' || (local[0] && local[1] == ':');
    const char *slash = NULL;
    if (doc_path) for (const char *q = doc_path; *q; ++q) if (*q == '/' || *q == '\\') slash = q;
    if (absolute || !slash) path = SDL_strdup(local);
    else SDL_asprintf(&path, "%.*s/%s", (int)(slash - doc_path), doc_path, local);
    if (is_markdown_path(path)) {
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
        if (e->key.key == SDLK_F1 && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            app.show_help = !app.show_help;
            app.dirty = true;
            break;
        }
        if (app.show_help) {
            /* Any other key just closes the help. */
            bool modifier = e->key.key == SDLK_LSHIFT || e->key.key == SDLK_RSHIFT ||
                            e->key.key == SDLK_LCTRL || e->key.key == SDLK_RCTRL ||
                            e->key.key == SDLK_LALT || e->key.key == SDLK_RALT;
            if (!modifier) {
                app.show_help = false;
                app.dirty = true;
            }
            break;
        }
        if (app.searching) {
            search_key(&e->key);
            break;
        }
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
        if (app.show_help) break;
        if (app.searching) {
            search_text(e->text.text);
            break;
        }
        if (app.focus == PANE_VIEWER || !app.nvim_ok) {
            if (SDL_strcmp(e->text.text, "/") == 0) search_begin(false);
            else if (SDL_strcmp(e->text.text, "?") == 0) search_begin(true);
            break;
        }
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
            app.viewer_scrolled = true;
            app.dirty = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (app.show_help) {
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                app.show_help = false;
                app.dirty = true;
            }
            break;
        }
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
                if (y < vr->y || y > vr->y + vr->h) {
                    view_scroll_by(&app.view, y < vr->y ? y - vr->y : y - (vr->y + vr->h));
                    app.viewer_scrolled = true;
                }
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

/* Viewer -> editor scroll sync, once per batch of input events. */
static void sync_editor_to_viewer(void)
{
    app.viewer_scrolled = false;
    if (!editor_visible() || !viewer_visible() || !app.nvim.attached) return;
    int line = view_top_line(&app.view);
    if (line == app.sent_top && line == app.nvim.top_line) return;
    app.sent_top = line;
    app.sent_at = SDL_GetTicks();
    app.nvim.top_line = line;
    nvim_scroll_to_line(&app.nvim, line);
}

/* -------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    /* md [-e | --edit] [file.md] */
    const char *file = NULL;
    bool edit = false;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "-e") == 0 || SDL_strcmp(argv[i], "--edit") == 0) edit = true;
        else if (!file) file = argv[i];
    }

    /* 1. Kick off background work before touching the window system. */
    app.font_thread = SDL_CreateThread(font_thread_main, "md-fonts", NULL);
    if (file) start_load(file);
    app.start_file = file;
    if (edit) begin_nvim(file); /* overlaps with window creation */

    /* 2. Window + renderer. */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
        return 1;
    }
    app.wake_event = SDL_RegisterEvents(1);
    images_init(app.wake_event);
    app.dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    /* Open as a viewer; Ctrl+Shift+S (or Ctrl+Shift+E) starts Neovim and
     * brings in the editor. */
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
    if (edit) { /* paint the split right away; nvim fills it in later */
        app.layout = LAYOUT_SPLIT;
        app.nvim_ok = true;
    }
    relayout();
    render();
    SDL_ShowWindow(app.window);
    app.nvim_ok = false;
    app.layout = LAYOUT_VIEWER;

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

    if (edit) set_layout(LAYOUT_VIEWER); /* toggles to the split, focus on the editor */
    else relayout();
    take_loaded_document(); /* in case the loader already finished */
    render();

    /* 5. Event loop: sleep until something happens. */
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
        if (running && app.viewer_scrolled) sync_editor_to_viewer();
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
