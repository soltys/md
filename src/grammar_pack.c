#include "grammar_pack.h"
#include "timing.h"

#include <SDL3/SDL.h>
#include <zstd.h>

int md_grammar_unpack(MdGrammarPack *pack, size_t size)
{
    SDL_AtomicInt *state = &pack->state;
    for (;;) {
        int s = SDL_GetAtomicInt(state);
        if (s == 2) return 1;
        if (s == 3) return 0;
        if (s == 0 && SDL_CompareAndSwapAtomicInt(state, 0, 1)) break;
        SDL_Delay(1); /* another thread is unpacking it */
    }
    TIMING_MARK("(work before unpacking)");
    const size_t a = pack->parse_table_size, b = pack->small_parse_table_size;
    unsigned char *raw = SDL_malloc(a + b);
    size_t got = raw ? ZSTD_decompress(raw, a + b, pack->data, size) : 0;
    const int ok = raw && !ZSTD_isError(got) && got == a + b;
    if (ok) {
        SDL_memcpy(pack->parse_table, raw, a);
        SDL_memcpy(pack->small_parse_table, raw + a, b);
    }
    SDL_free(raw);
    TIMING_EVENT("unpack %s parse tables (%.2f -> %.2f MB)", pack->name,
                 (double)size / (1024.0 * 1024.0), (double)(a + b) / (1024.0 * 1024.0));
    SDL_SetAtomicInt(state, ok ? 2 : 3);
    return ok;
}
