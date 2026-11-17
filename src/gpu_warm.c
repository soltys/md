#include "gpu_warm.h"

#ifdef _WIN32

#include "timing.h"

#include <SDL3/SDL.h>
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>

static SDL_Semaphore *release_device;

static int SDLCALL warm_main(void *unused)
{
    (void)unused;
    TIMING_THREAD("gpu");
    ID3D11Device *device = NULL;
    HMODULE dll = LoadLibraryW(L"d3d11.dll");
    TIMING_MARK("load d3d11.dll");
    PFN_D3D11_CREATE_DEVICE create =
        dll ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(dll, "D3D11CreateDevice") : NULL;
    if (create) {
        /* The same adapter (the default one), driver type and flags as
         * SDL's Direct3D 11 renderer, so the same user-mode driver loads. */
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
            D3D_FEATURE_LEVEL_9_1,
        };
        HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            levels, (UINT)SDL_arraysize(levels), D3D11_SDK_VERSION, &device,
                            NULL, NULL);
        if (hr == E_INVALIDARG) { /* runtimes that do not know 11_1 */
            hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                        levels + 1, (UINT)SDL_arraysize(levels) - 1, D3D11_SDK_VERSION, &device,
                        NULL, NULL);
        }
        if (FAILED(hr)) device = NULL;
    }
    TIMING_MARK("warm-up Direct3D 11 device %s (loads the GPU driver)", device ? "created" : "failed");
    /* Keep it (and with it the driver) alive until SDL has its own. */
    SDL_WaitSemaphore(release_device);
    if (device) ID3D11Device_Release(device);
    return 0;
}

void gpu_warm_start(void)
{
    if (release_device) return;
    release_device = SDL_CreateSemaphore(0);
    if (!release_device) return;
    SDL_Thread *t = SDL_CreateThread(warm_main, "md-gpu", NULL);
    if (t) {
        SDL_DetachThread(t);
    } else {
        SDL_DestroySemaphore(release_device);
        release_device = NULL;
    }
}

void gpu_warm_finish(void)
{
    /* The semaphore is left for the (detached) thread; it is tiny. */
    if (release_device) SDL_SignalSemaphore(release_device);
}

#else

void gpu_warm_start(void) {}
void gpu_warm_finish(void) {}

#endif
