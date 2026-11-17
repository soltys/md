#ifndef MD_GRAMMAR_PACK_H
#define MD_GRAMMAR_PACK_H

/* The highlighting grammars' parse tables are stored zstd-compressed (see
 * tools/ts_pack.c and cmake/Grammars.cmake). Each grammar's
 * tree_sitter_<id>() calls this before returning its language: the first
 * call unpacks the tables into the grammar's (until then empty) arrays,
 * later calls return at once. Thread-safe. Returns 0 on failure. */
#include <stddef.h>
#include <SDL3/SDL_atomic.h>

typedef struct MdGrammarPack {
    SDL_AtomicInt state;        /* 0 = packed, 1 = unpacking, 2 = ready, 3 = failed */
    const unsigned char *data;  /* the zstd frame */
    void *parse_table;          /* destinations, filled in this order */
    size_t parse_table_size;
    void *small_parse_table;
    size_t small_parse_table_size;
    const char *name;
} MdGrammarPack;

int md_grammar_unpack(MdGrammarPack *pack, size_t size);

#endif
