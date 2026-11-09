/*
 * md: a Markdown viewer/editor tuned for cold start.
 *
 * Startup is arranged so that the slow parts overlap:
 *   main thread     SDL_Init -> window + renderer -> first (empty) frame -> show
 *   font thread     TTF_Init, FreeType/HarfBuzz, open Roboto + Noto Color Emoji
 *   loader thread   read file, tree-sitter parse, build block list
 * The main thread only blocks on the font thread right before it needs to
 * draw text, by which time the window is already on screen.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "app.h"
#include "document.h"
#include "fonts.h"
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

static int SDLCALL loader_thread_main(void *arg)
{
    char *path = arg;
    Document *doc = document_load(path);
    SDL_free(path);
    /* Publish; if an older load was never picked up, discard it. */
    Document *stale = SDL_SetAtomicPointer(&app.loaded_doc, doc);
    document_free(stale);
    SDL_AddAtomicInt(&app.loads_pending, -1);
    wake_main_thread();
    return 0;
}

static void start_load(const char *path)
{
    SDL_AddAtomicInt(&app.loads_pending, 1);
    SDL_Thread *t = SDL_CreateThread(loader_thread_main, "md-loader", SDL_strdup(path));
    if (t) {
        SDL_DetachThread(t);
    } else {
        SDL_AddAtomicInt(&app.loads_pending, -1);
    }
}

static void SDLCALL open_dialog_done(void *userdata, const char *const *files, int filter)
{
    (void)userdata;
    (void)filter;
    if (files && files[0]) start_load(files[0]);
}

static void show_open_dialog(void)
{
    static const SDL_DialogFileFilter filters[] = {
        {"Markdown", "md;markdown;mdown;mkd;mkdn;txt"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(open_dialog_done, NULL, app.window, filters, 2, NULL, false);
}

/* ------------------------------------------------------------- frame -- */

static float display_scale(void)
{
    float s = SDL_GetWindowDisplayScale(app.window);
    return s > 0 ? s : 1.0f;
}

static void relayout(void)
{
    int w = 0, h = 0;
    SDL_GetRenderOutputSize(app.renderer, &w, &h);
    view_layout(&app.view, (float)w, (float)h, display_scale());
    app.dirty = true;
}

static void update_title(void)
{
    Document *doc = app.view.doc;
    if (!doc) {
        SDL_SetWindowTitle(app.window, APP_NAME);
        return;
    }
    const char *name = doc->path;
    for (const char *p = doc->path; *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    char *title = NULL;
    SDL_asprintf(&title, "%s - " APP_NAME, name);
    SDL_SetWindowTitle(app.window, title ? title : APP_NAME);
    SDL_free(title);
}

static void take_loaded_document(void)
{
    if (!app.fonts_ok) return;
    Document *doc = SDL_SetAtomicPointer(&app.loaded_doc, NULL);
    if (!doc) return;
    view_set_document(&app.view, doc);
    update_title();
    relayout();
}

static void render(void)
{
    const Theme *th = &app.view.theme;
    SDL_SetRenderDrawColor(app.renderer, th->bg.r, th->bg.g, th->bg.b, 0xFF);
    SDL_RenderClear(app.renderer);

    if (app.fonts_ok) {
        Document *doc = app.view.doc;
        if (doc && doc->error) {
            view_draw_message(&app.view, app.renderer, "Could not open the file", doc->error);
        } else if (doc && doc->block_count > 0) {
            view_draw(&app.view, app.renderer);
        } else if (doc) {
            view_draw_message(&app.view, app.renderer, "Empty document", doc->path);
        } else if (SDL_GetAtomicInt(&app.loads_pending) == 0) {
            view_draw_message(&app.view, app.renderer, "Drop a Markdown file here",
                              "or press Ctrl+O to open one");
        }
    }
    SDL_RenderPresent(app.renderer);
    app.dirty = false;
}

/* ------------------------------------------------------------ events -- */

static bool handle_key(const SDL_KeyboardEvent *k)
{
    const bool ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    View *v = &app.view;
    const float line = view_line_height(v);
    const float page = SDL_max(v->height - 2 * line, line);

    switch (k->key) {
    case SDLK_O:        if (ctrl) show_open_dialog(); return true;
    case SDLK_Q:
    case SDLK_W:        return !ctrl; /* Ctrl+Q / Ctrl+W quit */
    case SDLK_UP:       view_scroll_by(v, -line); break;
    case SDLK_DOWN:     view_scroll_by(v, line); break;
    case SDLK_PAGEUP:   view_scroll_by(v, -page); break;
    case SDLK_PAGEDOWN: view_scroll_by(v, page); break;
    case SDLK_SPACE:    view_scroll_by(v, (k->mod & SDL_KMOD_SHIFT) ? -page : page); break;
    case SDLK_HOME:     view_scroll_to(v, 0); break;
    case SDLK_END:      view_scroll_to(v, v->content_height); break;
    default:            return true;
    }
    app.dirty = true;
    return true;
}

/* Returns false when the app should quit. */
static bool handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT:
        return false;
    case SDL_EVENT_KEY_DOWN:
        return handle_key(&e->key);
    case SDL_EVENT_MOUSE_WHEEL:
        view_scroll_by(&app.view, -e->wheel.y * 3 * view_line_height(&app.view));
        app.dirty = true;
        break;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) start_load(e->drop.data);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if (app.fonts_ok) relayout();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        app.dirty = true;
        break;
    case SDL_EVENT_SYSTEM_THEME_CHANGED: {
        bool dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
        if (dark != app.dark) {
            app.dark = dark;
            /* Colours are baked into texts at creation; rebuild them. */
            Document *doc = app.view.doc;
            app.view.doc = NULL;
            app.view.theme = dark ? THEME_DARK : THEME_LIGHT;
            view_set_document(&app.view, doc);
            relayout();
        }
        break;
    }
    default:
        if (e->type == app.wake_event) take_loaded_document();
        break;
    }
    return true;
}

/* -------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    /* 1. Kick off background work before touching the window system. */
    app.font_thread = SDL_CreateThread(font_thread_main, "md-fonts", NULL);
    if (argc > 1) start_load(argv[1]);

    /* 2. Window + renderer. */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
        return 1;
    }
    app.wake_event = SDL_RegisterEvents(1);
    app.dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;

    if (!SDL_CreateWindowAndRenderer(APP_NAME, 1100, 820,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                     SDL_WINDOW_HIDDEN,
                                     &app.window, &app.renderer)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, SDL_GetError(), NULL);
        return 1;
    }

    /* 3. First frame: just the background colour, then show the window.
     *    Presenting before showing avoids a white/black flash. */
    const Theme *theme = app.dark ? &THEME_DARK : &THEME_LIGHT;
    SDL_SetRenderDrawColor(app.renderer, theme->bg.r, theme->bg.g, theme->bg.b, 0xFF);
    SDL_RenderClear(app.renderer);
    SDL_RenderPresent(app.renderer);
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
    if (app.fonts_ok) {
        relayout();
        take_loaded_document(); /* in case the loader already finished */
    }
    render();

    /* 5. Event loop: sleep until something happens. */
    bool running = true;
    while (running) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) break;
        do {
            running = handle_event(&e);
        } while (running && SDL_PollEvent(&e));
        if (running && app.dirty) render();
    }

    view_destroy(&app.view);
    document_free(SDL_SetAtomicPointer(&app.loaded_doc, NULL));
    if (app.engine) TTF_DestroyRendererTextEngine(app.engine);
    if (app.fonts_ok) fonts_shutdown();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
