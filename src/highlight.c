#include "highlight.h"
#include "regex.h"

#include <SDL3/SDL.h>
#include <tree_sitter/api.h>

#define GRAMMAR(id) \
    const TSLanguage *tree_sitter_##id(void); \
    extern const unsigned char md_query_##id[]; \
    extern const size_t md_query_##id##_len;
GRAMMAR(c)
GRAMMAR(cpp)
GRAMMAR(python)
GRAMMAR(javascript)
GRAMMAR(json)
GRAMMAR(bash)
GRAMMAR(rust)
GRAMMAR(c_sharp)
GRAMMAR(lua)

typedef struct Lang {
    const char *aliases;     /* space-separated, lower case */
    const TSLanguage *(*language)(void);
    const unsigned char *query;
    const size_t *query_len;
    const unsigned char *base_query;  /* prepended (C++ inherits C) */
    const size_t *base_query_len;
    void *compiled;          /* TSQuery*, set once (atomic) */
    void *classes;           /* uint8_t per capture id */
} Lang;

#define L(id, aliases) {aliases, tree_sitter_##id, md_query_##id, &md_query_##id##_len, NULL, NULL, NULL, NULL}
static Lang langs[] = {
    L(c, "c h"),
    {"cpp c++ cc cxx hpp hh h++", tree_sitter_cpp, md_query_cpp, &md_query_cpp_len,
     md_query_c, &md_query_c_len, NULL, NULL},
    L(python, "python py python3 py3 gyp"),
    L(javascript, "javascript js jsx mjs cjs node"),
    L(json, "json jsonc json5 geojson"),
    L(bash, "bash sh shell zsh console shellscript"),
    L(rust, "rust rs"),
    L(c_sharp, "csharp cs c# c-sharp"),
    L(lua, "lua luau"),
};

static Lang *find_lang(const char *name, size_t n)
{
    char key[32];
    if (n == 0 || n >= sizeof(key)) return NULL;
    for (size_t i = 0; i < n; ++i) key[i] = (char)SDL_tolower((unsigned char)name[i]);
    key[n] = 0;
    for (size_t i = 0; i < SDL_arraysize(langs); ++i) {
        const char *a = langs[i].aliases;
        while (*a) {
            const char *e = a;
            while (*e && *e != ' ') e++;
            if ((size_t)(e - a) == n && SDL_memcmp(a, key, n) == 0) return &langs[i];
            a = *e ? e + 1 : e;
        }
    }
    return NULL;
}

static uint8_t class_for(const char *name, uint32_t n)
{
    #define PFX(p) (n >= sizeof(p) - 1 && SDL_memcmp(name, p, sizeof(p) - 1) == 0)
    if (PFX("comment")) return HL_COMMENT;
    if (PFX("string") || PFX("escape") || PFX("character")) return HL_STRING;
    if (PFX("keyword") || PFX("repeat") || PFX("conditional") || PFX("include") ||
        PFX("preproc") || PFX("exception") || PFX("storageclass") || PFX("label")) return HL_KEYWORD;
    if (PFX("number") || PFX("float") || PFX("boolean")) return HL_NUMBER;
    if (PFX("constant")) return HL_CONSTANT;
    if (PFX("function") || PFX("method") || PFX("constructor")) return HL_FUNCTION;
    if (PFX("type") || PFX("module") || PFX("namespace")) return HL_TYPE;
    if (PFX("variable.builtin")) return HL_VARIABLE_BUILTIN;
    if (PFX("operator")) return HL_OPERATOR;
    if (PFX("punctuation") || PFX("delimiter")) return HL_PUNCTUATION;
    if (PFX("property") || PFX("field")) return HL_PROPERTY;
    if (PFX("tag")) return HL_TAG;
    if (PFX("attribute")) return HL_ATTRIBUTE;
    return HL_NONE;
    #undef PFX
}

/* Compiles a language's query once; safe to race (loser frees its copy). */
static TSQuery *get_query(Lang *l, uint8_t **classes)
{
    TSQuery *q = SDL_GetAtomicPointer(&l->compiled);
    if (q) {
        *classes = SDL_GetAtomicPointer(&l->classes);
        return q;
    }
    size_t base_len = l->base_query ? *l->base_query_len : 0;
    size_t len = *l->query_len;
    char *src = SDL_malloc(base_len + len + 2);
    if (base_len) SDL_memcpy(src, l->base_query, base_len);
    src[base_len] = '\n';
    SDL_memcpy(src + base_len + 1, l->query, len);
    src[base_len + 1 + len] = 0;

    uint32_t err_off;
    TSQueryError err;
    q = ts_query_new(l->language(), src, (uint32_t)(base_len + 1 + len), &err_off, &err);
    if (!q && base_len) { /* base query not compatible: use the language's own */
        q = ts_query_new(l->language(), (const char *)l->query, (uint32_t)len, &err_off, &err);
    }
    SDL_free(src);
    if (!q) return NULL;

    uint32_t nc = ts_query_capture_count(q);
    uint8_t *cls = SDL_malloc(nc ? nc : 1);
    for (uint32_t i = 0; i < nc; ++i) {
        uint32_t n;
        const char *name = ts_query_capture_name_for_id(q, i, &n);
        cls[i] = class_for(name, n);
    }
    SDL_SetAtomicPointer(&l->classes, cls);
    if (!SDL_CompareAndSwapAtomicPointer(&l->compiled, NULL, q)) {
        ts_query_delete(q);
        q = SDL_GetAtomicPointer(&l->compiled);
    }
    *classes = SDL_GetAtomicPointer(&l->classes);
    return q;
}

static const TSNode *capture_node(const TSQueryMatch *m, uint32_t id)
{
    for (uint16_t i = 0; i < m->capture_count; ++i) {
        if (m->captures[i].index == id) return &m->captures[i].node;
    }
    return NULL;
}

static bool node_text_eq(const char *code, TSNode n, const char *s, uint32_t len)
{
    uint32_t a = ts_node_start_byte(n), b = ts_node_end_byte(n);
    return b - a == len && SDL_memcmp(code + a, s, len) == 0;
}

/* Evaluates #eq? #not-eq? #match? #not-match? #any-of?; others pass. */
static bool predicates_ok(TSQuery *q, const TSQueryMatch *m, const char *code)
{
    uint32_t steps;
    const TSQueryPredicateStep *p = ts_query_predicates_for_pattern(q, m->pattern_index, &steps);
    uint32_t i = 0;
    while (i < steps) {
        uint32_t j = i;
        while (j < steps && p[j].type != TSQueryPredicateStepTypeDone) j++;
        /* predicate is p[i..j) */
        if (j - i >= 2 && p[i].type == TSQueryPredicateStepTypeString) {
            uint32_t nl;
            const char *name = ts_query_string_value_for_id(q, p[i].value_id, &nl);
            bool neg = nl > 4 && SDL_memcmp(name, "not-", 4) == 0;
            const char *base = neg ? name + 4 : name;
            const TSNode *node = p[i + 1].type == TSQueryPredicateStepTypeCapture
                                     ? capture_node(m, p[i + 1].value_id) : NULL;
            if (node) {
                bool result = true;
                if (SDL_strncmp(base, "eq?", 3) == 0 && j - i >= 3) {
                    if (p[i + 2].type == TSQueryPredicateStepTypeString) {
                        uint32_t sl;
                        const char *s = ts_query_string_value_for_id(q, p[i + 2].value_id, &sl);
                        result = node_text_eq(code, *node, s, sl);
                    } else {
                        const TSNode *other = capture_node(m, p[i + 2].value_id);
                        if (other) {
                            uint32_t a = ts_node_start_byte(*other), b = ts_node_end_byte(*other);
                            result = node_text_eq(code, *node, code + a, b - a);
                        }
                    }
                } else if (SDL_strncmp(base, "match?", 6) == 0 && j - i >= 3 &&
                           p[i + 2].type == TSQueryPredicateStepTypeString) {
                    uint32_t sl;
                    const char *pat = ts_query_string_value_for_id(q, p[i + 2].value_id, &sl);
                    uint32_t a = ts_node_start_byte(*node), b = ts_node_end_byte(*node);
                    int r = regex_search(pat, code + a, b - a);
                    if (r < 0) return false; /* unsupported pattern: skip the match */
                    result = r == 1;
                } else if (SDL_strncmp(base, "any-of?", 7) == 0) {
                    result = false;
                    for (uint32_t k = i + 2; k < j; ++k) {
                        uint32_t sl;
                        const char *s = ts_query_string_value_for_id(q, p[k].value_id, &sl);
                        if (node_text_eq(code, *node, s, sl)) { result = true; break; }
                    }
                } else if (SDL_strncmp(base, "lua-match?", 10) == 0) {
                    return false; /* Lua patterns are not supported */
                }
                if (neg) result = !result;
                if (!result) return false;
            }
        }
        i = j + 1;
    }
    return true;
}

/* ---------------------------------------------------------- cache -- */

/* Recent results by (language, code). Parser threads can overlap (file load
 * and live preview), hence the lock. */
#define CACHE_SLOTS 64
typedef struct CacheEntry { uint64_t key; size_t len; Span *spans; uint32_t count; } CacheEntry;
static CacheEntry cache[CACHE_SLOTS];
static uint32_t cache_next;
static SDL_SpinLock cache_lock;

static uint64_t cache_key(const Lang *l, const char *code, size_t len)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(uintptr_t)l;
    for (size_t i = 0; i < len; ++i) { h ^= (unsigned char)code[i]; h *= 1099511628211ull; }
    return h;
}

static bool cache_lookup(uint64_t key, size_t len, Span **spans, uint32_t *count, uint32_t *cap)
{
    bool hit = false;
    SDL_LockSpinlock(&cache_lock);
    for (int i = 0; i < CACHE_SLOTS; ++i) {
        const CacheEntry *e = &cache[i];
        if (!e->spans || e->key != key || e->len != len) continue;
        if (*count + e->count > *cap) {
            while (*count + e->count > *cap) *cap = *cap ? *cap * 2 : 256;
            *spans = SDL_realloc(*spans, sizeof(Span) * (size_t)*cap);
        }
        SDL_memcpy(*spans + *count, e->spans, sizeof(Span) * e->count);
        *count += e->count;
        hit = true;
        break;
    }
    SDL_UnlockSpinlock(&cache_lock);
    return hit;
}

static void cache_store(uint64_t key, size_t len, const Span *spans, uint32_t count)
{
    Span *copy = SDL_malloc(sizeof(Span) * (count ? count : 1));
    if (!copy) return;
    if (count) SDL_memcpy(copy, spans, sizeof(Span) * count);
    SDL_LockSpinlock(&cache_lock);
    CacheEntry *e = &cache[cache_next++ % CACHE_SLOTS];
    Span *old = e->spans;
    *e = (CacheEntry){key, len, copy, count};
    SDL_UnlockSpinlock(&cache_lock);
    SDL_free(old);
}

void highlight_code(const char *lang, size_t lang_len, const char *code, size_t len,
                    Span **spans, uint32_t *count, uint32_t *cap)
{
    if (len == 0 || len > (1u << 22)) return;
    /* Info strings may carry extra words ("c title=x"); use the first. */
    size_t n = 0;
    while (n < lang_len && lang[n] != ' ' && lang[n] != '{' && lang[n] != ',') n++;
    Lang *l = find_lang(lang, n);
    if (!l) return;
    uint8_t *classes = NULL;
    TSQuery *q = get_query(l, &classes);
    if (!q) return;

    /* The live preview re-parses the whole document on every edit, but
     * usually only one code block (if any) changed: reuse the others. */
    const uint64_t key = cache_key(l, code, len);
    if (cache_lookup(key, len, spans, count, cap)) return;
    const uint32_t first = *count;

    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, l->language());
    TSTree *tree = ts_parser_parse_string(parser, NULL, code, (uint32_t)len);
    ts_parser_delete(parser);
    if (!tree) return;

    /* Per byte: class, plus the size and pattern of the capture that set it.
     * Smaller (inner) captures win; on equal size the earlier pattern wins. */
    uint8_t *cls = SDL_calloc(len, 1);
    uint32_t *size = SDL_malloc(len * sizeof(uint32_t));
    uint16_t *pat = SDL_malloc(len * sizeof(uint16_t));
    for (size_t i = 0; i < len; ++i) { size[i] = UINT32_MAX; pat[i] = UINT16_MAX; }

    TSQueryCursor *cur = ts_query_cursor_new();
    ts_query_cursor_exec(cur, q, ts_tree_root_node(tree));
    TSQueryMatch m;
    while (ts_query_cursor_next_match(cur, &m)) {
        if (!predicates_ok(q, &m, code)) continue;
        for (uint16_t i = 0; i < m.capture_count; ++i) {
            uint8_t c = classes[m.captures[i].index];
            uint32_t a = ts_node_start_byte(m.captures[i].node);
            uint32_t b = ts_node_end_byte(m.captures[i].node);
            if (b > len) b = (uint32_t)len;
            uint32_t sz = b - a;
            for (uint32_t k = a; k < b; ++k) {
                if (sz < size[k] || (sz == size[k] && m.pattern_index < pat[k])) {
                    cls[k] = c;
                    size[k] = sz;
                    pat[k] = m.pattern_index;
                }
            }
        }
    }
    ts_query_cursor_delete(cur);
    ts_tree_delete(tree);

    /* Runs of the same class become spans. */
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && cls[j] == cls[i]) j++;
        if (cls[i] != HL_NONE) {
            if (*count == *cap) {
                *cap = *cap ? *cap * 2 : 256;
                *spans = SDL_realloc(*spans, sizeof(Span) * (size_t)*cap);
            }
            Span *s = &(*spans)[(*count)++];
            SDL_zerop(s);
            s->start = (uint32_t)i;
            s->len = (uint32_t)(j - i);
            s->color = cls[i];
        }
        i = j;
    }
    SDL_free(cls);
    SDL_free(size);
    SDL_free(pat);
    cache_store(key, len, *spans + first, *count - first);
}
