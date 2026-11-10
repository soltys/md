#include "assets.h"

#ifdef _WIN32
#include <windows.h>

bool asset_get(int id, const void **data, size_t *size)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) return false;
    HGLOBAL h = LoadResource(NULL, res);
    if (!h) return false;
    *data = LockResource(h);
    *size = SizeofResource(NULL, res);
    return *data != NULL && *size > 0;
}

#else /* Development fallback for non-Windows sanity builds. */
#include <SDL3/SDL.h>

static const char *asset_path(int id)
{
    switch (id) {
    case ASSET_ROBOTO_REGULAR:     return MD_ASSET_ROBOTO_REGULAR;
    case ASSET_ROBOTO_ITALIC:      return MD_ASSET_ROBOTO_ITALIC;
    case ASSET_ROBOTO_MEDIUM:      return MD_ASSET_ROBOTO_MEDIUM;
    case ASSET_ROBOTO_BOLD:        return MD_ASSET_ROBOTO_BOLD;
    case ASSET_ROBOTO_BOLD_ITALIC: return MD_ASSET_ROBOTO_BOLD_ITALIC;
    case ASSET_ROBOTO_LIGHT:       return MD_ASSET_ROBOTO_LIGHT;
    case ASSET_ROBOTO_MONO:        return MD_ASSET_ROBOTO_MONO;
    case ASSET_ROBOTO_MONO_ITALIC: return MD_ASSET_ROBOTO_MONO_ITALIC;
    case ASSET_NOTO_EMOJI:         return MD_ASSET_NOTO_EMOJI;
    case ASSET_NVIM_INIT:          return MD_ASSET_NVIM_INIT;
    case ASSET_ROBOTO_MONO_BOLD:   return MD_ASSET_ROBOTO_MONO_BOLD;
    case ASSET_ROBOTO_MONO_BOLD_ITALIC: return MD_ASSET_ROBOTO_MONO_BOLD_ITALIC;
    case ASSET_NOTO_MATH:          return MD_ASSET_NOTO_MATH;
    case ASSET_NOTO_SYMBOLS2:      return MD_ASSET_NOTO_SYMBOLS2;
    default:                       return NULL;
    }
}

bool asset_get(int id, const void **data, size_t *size)
{
    static struct { const void *data; size_t size; } cache[16];
    int slot = id - ASSET_ROBOTO_REGULAR;
    if (slot < 0 || slot >= 16) return false;
    if (!cache[slot].data) {
        const char *path = asset_path(id);
        if (!path) return false;
        cache[slot].data = SDL_LoadFile(path, &cache[slot].size);
        if (!cache[slot].data) return false;
    }
    *data = cache[slot].data;
    *size = cache[slot].size;
    return true;
}
#endif
