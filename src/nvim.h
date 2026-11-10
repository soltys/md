#ifndef MD_NVIM_H
#define MD_NVIM_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "msgpack.h"

/* The editor pane's Neovim: `nvim --embed -u <config> [file]`, driven over
 * msgpack-RPC on its stdin/stdout as an external UI (ext_linegrid).
 *
 * The config is nvim/init.lua from the repo, embedded in md.exe and written
 * to <SDL pref path>/init.lua (only when its content changed). With --embed
 * Neovim waits for a UI to attach before sourcing the config, so the process
 * is started on demand, the first time the editor is shown. */

typedef struct NvCell {
    char text[28];      /* UTF-8 grapheme; "" = right half of a wide char */
    uint8_t len;
    int hl;
} NvCell;

typedef struct NvHl {
    uint32_t fg, bg, sp;
    bool has_fg, has_bg, has_sp;
    bool bold, italic, underline, undercurl, strikethrough, reverse;
} NvHl;

enum { NV_CURSOR_BLOCK, NV_CURSOR_VERTICAL, NV_CURSOR_HORIZONTAL };

typedef struct NvModeInfo {
    int shape;
    int percentage;
    int attr_id;
} NvModeInfo;

typedef struct Nvim {
    /* process */
    SDL_Process *process;
    SDL_IOStream *in;
    SDL_IOStream *out;
    char *config_path;
    char *error;

    /* reader thread -> main thread queue */
    SDL_Thread *reader;
    SDL_Mutex *lock;
    MpMsg *queue;
    int queue_len, queue_cap;
    SDL_AtomicInt exited;
    Uint32 wake_event;

    /* UI state, main thread only */
    bool attached;
    int rows, cols;
    NvCell *cells;
    bool *row_dirty;
    int cursor_row, cursor_col;
    NvHl *hls;
    int hl_count;
    uint32_t def_fg, def_bg, def_sp;
    NvModeInfo modes[32];
    int mode_count, mode_idx;
    bool busy;
    bool flushed;        /* a complete frame is ready to draw */
    bool colors_changed;

    /* RPC */
    uint32_t next_msgid;
    uint32_t api_info_request;
    uint32_t text_request;   /* msgid of the pending buffer-text request */
    bool text_changed;       /* md_changed seen, not yet requested */
    bool text_dirty_again;   /* changed again while a request was in flight */
    char *buf_name;          /* result of the last text request */
    char *buf_text;
    size_t buf_text_len;
    bool buf_ready;
    int top_line;            /* first visible buffer line (1-based), md_scroll */
    bool at_end;             /* the last buffer line is on screen (and not the first) */
    bool scrolled;
    char notice[128];        /* md_notice text for a toast, "" when none */
} Nvim;

/* Starts the process on a background thread. `file` may be NULL. */
void nvim_start_async(Nvim *nv, const char *file, Uint32 wake_event);

/* Waits for the background start to finish; true if nvim is running. */
bool nvim_wait_started(Nvim *nv);

/* Attaches md as Neovim's UI with the given grid size. */
bool nvim_attach(Nvim *nv, int cols, int rows);

/* Applies queued messages from Neovim. Returns true when anything that
 * affects drawing or the preview happened. Main thread only. */
bool nvim_process(Nvim *nv);

bool nvim_has_exited(Nvim *nv);

void nvim_resize(Nvim *nv, int cols, int rows);
void nvim_input(Nvim *nv, const char *keys);
void nvim_input_mouse(Nvim *nv, const char *button, const char *action,
                      const char *mods, int row, int col);
void nvim_edit_file(Nvim *nv, const char *path);
/* F5: re-reads the buffer's file unless it has unsaved changes. */
void nvim_reload(Nvim *nv);
/* Scrolls the editor so `line` (1-based) is at the top (viewer -> editor
 * sync); 0 shows the end of the file. */
void nvim_scroll_to_line(Nvim *nv, int line);
void nvim_request_quit(Nvim *nv);
/* Requests buffer name + text; the answer sets buf_ready. */
void nvim_request_text(Nvim *nv);

const NvHl *nvim_hl(const Nvim *nv, int id);

void nvim_stop(Nvim *nv);

#endif
