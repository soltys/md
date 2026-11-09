#include "document.h"

#include <SDL3/SDL.h>
#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);

typedef struct Ctx {
    uint8_t list_depth;
    uint8_t quote_depth;
    /* Marker of the enclosing list item, consumed by its first block. */
    const char *marker;
    uint32_t marker_len;
    uint8_t task;
} Ctx;

/* ------------------------------------------------------------ strings -- */

static uint32_t str_begin(Document *doc)
{
    return (uint32_t)doc->strings_len;
}

static void str_append(Document *doc, const char *s, size_t n)
{
    if (doc->strings_len + n + 1 > doc->strings_cap) {
        size_t cap = doc->strings_cap ? doc->strings_cap : 4096;
        while (cap < doc->strings_len + n + 1) cap *= 2;
        doc->strings = SDL_realloc(doc->strings, cap);
        doc->strings_cap = cap;
    }
    SDL_memcpy(doc->strings + doc->strings_len, s, n);
    doc->strings_len += n;
}

static uint32_t str_end(Document *doc, uint32_t begin)
{
    uint32_t len = (uint32_t)doc->strings_len - begin;
    str_append(doc, "", 1); /* NUL terminator for convenience */
    return len;
}

/* -------------------------------------------------------------- nodes -- */

static bool is(TSNode n, const char *type)
{
    return SDL_strcmp(ts_node_type(n), type) == 0;
}

static TSNode child_of_type(TSNode n, const char *type)
{
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode c = ts_node_named_child(n, i);
        if (is(c, type)) return c;
    }
    TSNode none = {0};
    return none;
}

/* Copies the bytes of `n` into the string table, skipping nested
 * block_continuation nodes (the "> " or indentation repeated on every line of
 * a quoted / list-nested block). When `join_lines` is set, soft line breaks
 * become spaces, which is how paragraphs render. */
static void append_text(Document *doc, TSNode n, bool join_lines)
{
    uint32_t pos = ts_node_start_byte(n);
    uint32_t end = ts_node_end_byte(n);
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i <= count; ++i) {
        uint32_t stop = end;
        TSNode c = {0};
        if (i < count) {
            c = ts_node_child(n, i);
            if (!is(c, "block_continuation")) continue;
            stop = ts_node_start_byte(c);
        }
        for (uint32_t p = pos; p < stop && p < end; ++p) {
            char ch = doc->source[p];
            if (ch == '\r') continue;
            if (join_lines && ch == '\n') ch = ' ';
            str_append(doc, &ch, 1);
        }
        if (i < count) pos = ts_node_end_byte(c);
    }
}

static void trim_trailing(Document *doc, uint32_t begin)
{
    while (doc->strings_len > begin) {
        char ch = doc->strings[doc->strings_len - 1];
        if (ch != ' ' && ch != '\n' && ch != '\t') break;
        doc->strings_len--;
    }
}

static Block *push_block(Document *doc, BlockKind kind, TSNode n, Ctx *ctx)
{
    if (doc->block_count == doc->block_cap) {
        doc->block_cap = doc->block_cap ? doc->block_cap * 2 : 256;
        doc->blocks = SDL_realloc(doc->blocks, sizeof(Block) * (size_t)doc->block_cap);
    }
    Block *b = &doc->blocks[doc->block_count++];
    SDL_zerop(b);
    b->kind = kind;
    b->list_depth = ctx->list_depth;
    b->quote_depth = ctx->quote_depth;
    b->src_start = ts_node_start_byte(n);
    b->src_end = ts_node_end_byte(n);
    b->src_line = ts_node_start_point(n).row + 1;
    if (ctx->marker) {
        b->marker = str_begin(doc);
        str_append(doc, ctx->marker, ctx->marker_len);
        b->marker_len = str_end(doc, b->marker);
        b->task = ctx->task;
        ctx->marker = NULL;
        ctx->task = TASK_NONE;
    }
    return b;
}

static void add_text_block(Document *doc, BlockKind kind, TSNode n, TSNode content,
                           Ctx *ctx, bool join_lines)
{
    Block *b = push_block(doc, kind, n, ctx);
    uint32_t begin = str_begin(doc);
    if (!ts_node_is_null(content)) append_text(doc, content, join_lines);
    trim_trailing(doc, begin);
    /* push_block may have grown `blocks`; re-fetch by index. */
    b = &doc->blocks[doc->block_count - 1];
    b->text = begin;
    b->text_len = str_end(doc, begin);
}

static void walk(Document *doc, TSNode n, Ctx *ctx);

static void walk_children(Document *doc, TSNode n, Ctx *ctx)
{
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) walk(doc, ts_node_named_child(n, i), ctx);
}

static void walk_list_item(Document *doc, TSNode n, Ctx *parent)
{
    Ctx ctx = *parent;
    static const char bullet[] = "\xE2\x80\xA2"; /* U+2022 • */
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode c = ts_node_named_child(n, i);
        const char *t = ts_node_type(c);
        if (SDL_strncmp(t, "list_marker_", 12) == 0) {
            if (is(c, "list_marker_dot") || is(c, "list_marker_parenthesis")) {
                /* Ordered: keep the number as written, minus whitespace. */
                uint32_t s = ts_node_start_byte(c), e = ts_node_end_byte(c);
                while (s < e && (doc->source[s] == ' ' || doc->source[s] == '\t')) s++;
                while (e > s && (doc->source[e - 1] == ' ' || doc->source[e - 1] == '\t')) e--;
                ctx.marker = doc->source + s;
                ctx.marker_len = e - s;
            } else {
                ctx.marker = bullet;
                ctx.marker_len = sizeof(bullet) - 1;
            }
        } else if (is(c, "task_list_marker_checked")) {
            ctx.task = TASK_CHECKED;
        } else if (is(c, "task_list_marker_unchecked")) {
            ctx.task = TASK_UNCHECKED;
        } else if (!is(c, "block_continuation")) {
            walk(doc, c, &ctx);
        }
    }
    if (ctx.marker) {
        /* Empty list item: still show its marker. */
        TSNode none = {0};
        add_text_block(doc, BLOCK_PARAGRAPH, n, none, &ctx, true);
    }
}

static void walk(Document *doc, TSNode n, Ctx *ctx)
{
    const char *t = ts_node_type(n);

    if (is(n, "document") || is(n, "section")) {
        walk_children(doc, n, ctx);
    } else if (is(n, "block_quote")) {
        Ctx inner = *ctx;
        inner.quote_depth++;
        walk_children(doc, n, &inner);
        ctx->marker = inner.marker; /* consumed or not, propagate */
        ctx->task = inner.task;
    } else if (is(n, "list")) {
        Ctx inner = *ctx;
        inner.list_depth++;
        inner.marker = NULL;
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode c = ts_node_named_child(n, i);
            if (is(c, "list_item")) walk_list_item(doc, c, &inner);
        }
    } else if (is(n, "atx_heading")) {
        uint8_t level = 1;
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) {
            const char *ct = ts_node_type(ts_node_named_child(n, i));
            if (SDL_strncmp(ct, "atx_h", 5) == 0 && ct[5] >= '1' && ct[5] <= '6') {
                level = (uint8_t)(ct[5] - '0');
                break;
            }
        }
        add_text_block(doc, BLOCK_HEADING, n, child_of_type(n, "inline"), ctx, true);
        doc->blocks[doc->block_count - 1].level = level;
    } else if (is(n, "setext_heading")) {
        uint8_t level = ts_node_is_null(child_of_type(n, "setext_h2_underline")) ? 1 : 2;
        TSNode para = child_of_type(n, "paragraph");
        TSNode content = ts_node_is_null(para) ? para : child_of_type(para, "inline");
        add_text_block(doc, BLOCK_HEADING, n, content, ctx, true);
        doc->blocks[doc->block_count - 1].level = level;
    } else if (is(n, "paragraph")) {
        TSNode content = child_of_type(n, "inline");
        add_text_block(doc, BLOCK_PARAGRAPH, n, ts_node_is_null(content) ? n : content, ctx, true);
    } else if (is(n, "fenced_code_block")) {
        add_text_block(doc, BLOCK_CODE, n, child_of_type(n, "code_fence_content"), ctx, false);
        TSNode info = child_of_type(n, "info_string");
        if (!ts_node_is_null(info)) {
            TSNode lang = child_of_type(info, "language");
            TSNode src = ts_node_is_null(lang) ? info : lang;
            Block *b = &doc->blocks[doc->block_count - 1];
            b->lang = str_begin(doc);
            append_text(doc, src, true);
            b = &doc->blocks[doc->block_count - 1];
            b->lang_len = str_end(doc, b->lang);
        }
    } else if (is(n, "indented_code_block")) {
        add_text_block(doc, BLOCK_CODE, n, n, ctx, false);
    } else if (is(n, "thematic_break")) {
        push_block(doc, BLOCK_RULE, n, ctx);
    } else if (is(n, "pipe_table")) {
        add_text_block(doc, BLOCK_TABLE, n, n, ctx, false);
    } else if (is(n, "html_block")) {
        add_text_block(doc, BLOCK_HTML, n, n, ctx, false);
    } else if (is(n, "link_reference_definition") || is(n, "minus_metadata") ||
               is(n, "plus_metadata") || is(n, "block_continuation")) {
        /* not rendered */
    } else {
        (void)t;
        walk_children(doc, n, ctx);
    }
}

/* -------------------------------------------------------------- public -- */

static Document *fail(Document *doc, const char *what)
{
    SDL_asprintf(&doc->error, "%s: %s", what, SDL_GetError());
    return doc;
}

static Document *parse(Document *doc)
{
    if (doc->source_len > UINT32_MAX / 2) {
        SDL_asprintf(&doc->error, "File is too large");
        return doc;
    }
    uint32_t src_len = (uint32_t)doc->source_len;
    /* Blank out a UTF-8 BOM (keeps byte offsets stable). */
    if (src_len >= 3 && (unsigned char)doc->source[0] == 0xEF &&
        (unsigned char)doc->source[1] == 0xBB && (unsigned char)doc->source[2] == 0xBF) {
        SDL_memset(doc->source, ' ', 3);
    }

    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_markdown());
    TSTree *tree = ts_parser_parse_string(parser, NULL, doc->source, src_len);
    ts_parser_delete(parser);
    if (!tree) {
        SDL_asprintf(&doc->error, "Cannot parse file");
        return doc;
    }

    Ctx ctx = {0};
    walk(doc, ts_tree_root_node(tree), &ctx);
    ts_tree_delete(tree);
    return doc;
}

Document *document_load(const char *path)
{
    Document *doc = SDL_calloc(1, sizeof(Document));
    doc->path = SDL_strdup(path);
    doc->source = SDL_LoadFile(path, &doc->source_len);
    if (!doc->source) return fail(doc, "Cannot open file");
    return parse(doc);
}

Document *document_parse(const char *path, char *text, size_t len)
{
    Document *doc = SDL_calloc(1, sizeof(Document));
    doc->path = SDL_strdup(path ? path : "");
    doc->source = text ? text : SDL_strdup("");
    doc->source_len = text ? len : 0;
    return parse(doc);
}

void document_free(Document *doc)
{
    if (!doc) return;
    SDL_free(doc->path);
    SDL_free(doc->source);
    SDL_free(doc->strings);
    SDL_free(doc->blocks);
    SDL_free(doc->error);
    SDL_free(doc);
}
