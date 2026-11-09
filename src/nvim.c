#include "nvim.h"
#include "assets.h"

static SDL_Thread *start_thread;
static char *start_file;

/* Writes the embedded init.lua next to md's other per-user data, skipping
 * the write when the file on disk is already identical. */
static char *install_config(void)
{
    const void *data;
    size_t size;
    if (!asset_get(ASSET_NVIM_INIT, &data, &size)) {
        SDL_SetError("embedded nvim config missing");
        return NULL;
    }
    char *dir = SDL_GetPrefPath(NULL, "md");
    if (!dir) return NULL;
    char *path = NULL;
    SDL_asprintf(&path, "%sinit.lua", dir);
    SDL_free(dir);
    if (!path) return NULL;

    size_t old_size = 0;
    void *old = SDL_LoadFile(path, &old_size);
    bool same = old && old_size == size && SDL_memcmp(old, data, size) == 0;
    SDL_free(old);
    if (!same && !SDL_SaveFile(path, data, size)) {
        SDL_free(path);
        return NULL;
    }
    return path;
}

static int SDLCALL start_thread_main(void *userdata)
{
    Nvim *nv = userdata;

    nv->config_path = install_config();
    if (!nv->config_path) {
        SDL_asprintf(&nv->error, "Cannot write Neovim config: %s", SDL_GetError());
        return -1;
    }

    const char *args[6];
    int n = 0;
    args[n++] = "nvim";
    args[n++] = "--embed";
    args[n++] = "-u";
    args[n++] = nv->config_path;
    if (start_file) args[n++] = start_file;
    args[n] = NULL;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    /* No console window for nvim.exe. */
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    nv->process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);

    if (!nv->process) {
        SDL_asprintf(&nv->error, "Cannot start nvim (is it on PATH?): %s", SDL_GetError());
        return -1;
    }
    nv->in = SDL_GetProcessInput(nv->process);
    nv->out = SDL_GetProcessOutput(nv->process);
    return 0;
}

void nvim_start_async(Nvim *nv, const char *file)
{
    SDL_zerop(nv);
    start_file = file ? SDL_strdup(file) : NULL;
    start_thread = SDL_CreateThread(start_thread_main, "md-nvim", nv);
    if (!start_thread) start_thread_main(nv);
}

bool nvim_wait_started(Nvim *nv)
{
    if (start_thread) {
        SDL_WaitThread(start_thread, NULL);
        start_thread = NULL;
    }
    SDL_free(start_file);
    start_file = NULL;
    return nv->process != NULL;
}

void nvim_stop(Nvim *nv)
{
    nvim_wait_started(nv);
    if (nv->process) {
        /* The pipes are owned by the process object and closed with it. */
        SDL_KillProcess(nv->process, false);
        SDL_WaitProcess(nv->process, true, NULL);
        SDL_DestroyProcess(nv->process);
    }
    SDL_free(nv->config_path);
    SDL_free(nv->error);
    SDL_zerop(nv);
}
