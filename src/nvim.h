#ifndef MD_NVIM_H
#define MD_NVIM_H

#include <stdbool.h>
#include <SDL3/SDL.h>

/* The editor pane's Neovim process: `nvim --embed -u <config> [file]`.
 *
 * The config is nvim/init.lua from the repo, embedded in md.exe and written
 * to <SDL pref path>/init.lua (only when its content changed). With --embed
 * Neovim waits for a UI to attach before sourcing the config, so starting it
 * early costs nothing visible and hides process creation behind window
 * creation. */
typedef struct Nvim {
    SDL_Process *process;
    SDL_IOStream *in;   /* msgpack-RPC requests to nvim (its stdin) */
    SDL_IOStream *out;  /* responses / notifications (its stdout) */
    char *config_path;
    char *error;        /* set when it could not be started */
} Nvim;

/* Starts the process on a background thread. `file` may be NULL. */
void nvim_start_async(Nvim *nv, const char *file);

/* Waits for the background start to finish; true if nvim is running. */
bool nvim_wait_started(Nvim *nv);

void nvim_stop(Nvim *nv);

#endif
