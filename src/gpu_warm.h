#ifndef MD_GPU_WARM_H
#define MD_GPU_WARM_H

/* Loading the GPU driver is the slowest single step of md's start (100+ ms
 * inside SDL_CreateRenderer). gpu_warm_start() creates a throwaway
 * Direct3D 11 device on a helper thread right at startup, so the driver DLLs
 * are loaded and initialised while the main thread runs SDL_Init and
 * creates the window; SDL's own device then finds the driver ready.
 * gpu_warm_finish() releases the throwaway device once SDL's renderer
 * exists. Both are no-ops outside Windows and safe to call in any order. */
void gpu_warm_start(void);
void gpu_warm_finish(void);

#endif
