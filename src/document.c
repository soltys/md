#include "document.h"
#include "highlight.h"

#include <SDL3/SDL.h>
#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);

/* Inline content is collected during the block walk and parsed afterwards,
 * once all link reference definitions are known. */
typedef struct Pending {
    int block;            /* block index, or -1 for a table cell */
    uint32_t cell;
    uint32_t raw, raw_len;
} Pending;

typedef struct LinkDef {
    uint32_t label, label_len;  /* normalised */
    uint32_t url, url_len;
} LinkDef;

typedef struct Ctx {
    uint8_t list_depth;
    uint8_t quote_depth;
    const char *marker;   /* marker of the enclosing list item, consumed by its first block */
    uint32_t marker_len;
    uint8_t task;
} Ctx;

typedef struct Builder {
    Document *doc;
    Pending *pending;
    uint32_t pending_count, pending_cap;
    LinkDef *defs;
    uint32_t def_count, def_cap;
} Builder;

#define GROW(arr, count, cap, init)                                       \
    do {                                                                  \
        if ((count) == (cap)) {                                           \
            (cap) = (cap) ? (cap) * 2 : (init);                           \
            (arr) = SDL_realloc((arr), sizeof(*(arr)) * (size_t)(cap));   \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------ strings -- */

static uint32_t str_begin(Document *doc) { return (uint32_t)doc->strings_len; }

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
    str_append(doc, "", 1);
    return len;
}

static uint32_t str_add(Document *doc, const char *s, size_t n, uint32_t *len)
{
    uint32_t off = str_begin(doc);
    str_append(doc, s, n);
    *len = str_end(doc, off);
    return off;
}

/* -------------------------------------------------------------- nodes -- */

static bool is(TSNode n, const char *type) { return SDL_strcmp(ts_node_type(n), type) == 0; }

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

/* Copies the bytes of `n`, skipping nested block_continuation nodes (the
 * "> " or indentation repeated on each line of quoted / nested blocks).
 * Tabs become spaces in code (expand_tabs). */
static void append_text(Document *doc, TSNode n, bool expand_tabs)
{
    uint32_t pos = ts_node_start_byte(n);
    uint32_t end = ts_node_end_byte(n);
    uint32_t count = ts_node_child_count(n);
    uint32_t col = 0;
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
            if (ch == '\t' && expand_tabs) {
                do { str_append(doc, " ", 1); col++; } while (col % 4);
                continue;
            }
            col = ch == '\n' ? 0 : col + 1;
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

static Block *push_block(Builder *bd, BlockKind kind, TSNode n, Ctx *ctx)
{
    Document *doc = bd->doc;
    GROW(doc->blocks, doc->block_count, doc->block_cap, 256);
    Block *b = &doc->blocks[doc->block_count++];
    SDL_zerop(b);
    b->kind = kind;
    b->list_depth = ctx->list_depth;
    b->quote_depth = ctx->quote_depth;
    b->src_start = ts_node_start_byte(n);
    b->src_end = ts_node_end_byte(n);
    b->src_line = ts_node_start_point(n).row + 1;
    if (ctx->marker) {
        b->marker = str_add(doc, ctx->marker, ctx->marker_len, &b->marker_len);
        b->task = ctx->task;
        ctx->marker = NULL;
        ctx->task = TASK_NONE;
    }
    return b;
}

static void add_pending(Builder *bd, int block, uint32_t cell, uint32_t raw, uint32_t raw_len)
{
    GROW(bd->pending, bd->pending_count, bd->pending_cap, 256);
    Pending *p = &bd->pending[bd->pending_count++];
    p->block = block;
    p->cell = cell;
    p->raw = raw;
    p->raw_len = raw_len;
}

/* Block whose content is inline Markdown, parsed later. */
static void add_inline_block(Builder *bd, BlockKind kind, TSNode n, TSNode content, Ctx *ctx)
{
    Document *doc = bd->doc;
    push_block(bd, kind, n, ctx);
    uint32_t begin = str_begin(doc);
    if (!ts_node_is_null(content)) append_text(doc, content, false);
    trim_trailing(doc, begin);
    uint32_t len = str_end(doc, begin);
    int idx = doc->block_count - 1;
    doc->blocks[idx].text = begin;
    doc->blocks[idx].text_len = len;
    add_pending(bd, idx, 0, begin, len);
}

/* Block shown verbatim (code, HTML). */
static void add_raw_block(Builder *bd, BlockKind kind, TSNode n, TSNode content, Ctx *ctx)
{
    Document *doc = bd->doc;
    push_block(bd, kind, n, ctx);
    uint32_t begin = str_begin(doc);
    if (!ts_node_is_null(content)) append_text(doc, content, true);
    trim_trailing(doc, begin);
    uint32_t len = str_end(doc, begin);
    doc->blocks[doc->block_count - 1].text = begin;
    doc->blocks[doc->block_count - 1].text_len = len;
}

/* Lower-cased label with collapsed whitespace, for reference links. */
static uint32_t normalise_label(Document *doc, const char *s, size_t n, uint32_t *len)
{
    uint32_t off = str_begin(doc);
    bool space = false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '[' || c == ']') continue;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = true; continue; }
        if (space && doc->strings_len > off) str_append(doc, " ", 1);
        space = false;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        str_append(doc, &c, 1);
    }
    *len = str_end(doc, off);
    return off;
}

static void add_link_def(Builder *bd, TSNode n)
{
    Document *doc = bd->doc;
    TSNode label = child_of_type(n, "link_label");
    TSNode dest = child_of_type(n, "link_destination");
    if (ts_node_is_null(label) || ts_node_is_null(dest)) return;
    GROW(bd->defs, bd->def_count, bd->def_cap, 32);
    LinkDef *d = &bd->defs[bd->def_count++];
    uint32_t ls = ts_node_start_byte(label), le = ts_node_end_byte(label);
    d->label = normalise_label(doc, doc->source + ls, le - ls, &d->label_len);
    uint32_t us = ts_node_start_byte(dest), ue = ts_node_end_byte(dest);
    if (ue > us && doc->source[us] == '<') { us++; if (ue > us && doc->source[ue - 1] == '>') ue--; }
    d->url = str_add(doc, doc->source + us, ue - us, &d->url_len);
}

/* --------------------------------------------------------------- walk -- */

static void walk(Builder *bd, TSNode n, Ctx *ctx);

static void walk_children(Builder *bd, TSNode n, Ctx *ctx)
{
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) walk(bd, ts_node_named_child(n, i), ctx);
}

static void walk_list_item(Builder *bd, TSNode n, Ctx *parent)
{
    Document *doc = bd->doc;
    Ctx ctx = *parent;
    static const char bullet[] = "\xE2\x80\xA2"; /* U+2022 • */
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode c = ts_node_named_child(n, i);
        const char *t = ts_node_type(c);
        if (SDL_strncmp(t, "list_marker_", 12) == 0) {
            if (is(c, "list_marker_dot") || is(c, "list_marker_parenthesis")) {
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
            walk(bd, c, &ctx);
        }
    }
    if (ctx.marker) {
        TSNode none = {0};
        add_inline_block(bd, BLOCK_PARAGRAPH, n, none, &ctx);
    }
}

static void walk_table(Builder *bd, TSNode n, Ctx *ctx)
{
    Document *doc = bd->doc;
    push_block(bd, BLOCK_TABLE, n, ctx);
    int bi = doc->block_count - 1;
    doc->blocks[bi].cell_first = doc->cell_count;

    uint16_t row = 0, max_cols = 0;
    uint32_t count = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode r = ts_node_named_child(n, i);
        if (is(r, "pipe_table_delimiter_row")) {
            uint32_t aligns = 0, col = 0;
            uint32_t cc = ts_node_named_child_count(r);
            for (uint32_t k = 0; k < cc; ++k) {
                TSNode d = ts_node_named_child(r, k);
                if (!is(d, "pipe_table_delimiter_cell")) continue;
                bool left = !ts_node_is_null(child_of_type(d, "pipe_table_align_left"));
                bool right = !ts_node_is_null(child_of_type(d, "pipe_table_align_right"));
                uint32_t a = (left && right) ? ALIGN_CENTER : right ? ALIGN_RIGHT : ALIGN_LEFT;
                if (col < 16) aligns |= a << (2 * col);
                col++;
            }
            doc->blocks[bi].aligns = aligns;
            continue;
        }
        if (!is(r, "pipe_table_header") && !is(r, "pipe_table_row")) continue;
        uint16_t col = 0;
        uint32_t cc = ts_node_named_child_count(r);
        for (uint32_t k = 0; k < cc; ++k) {
            TSNode cell = ts_node_named_child(r, k);
            if (!is(cell, "pipe_table_cell")) continue;
            GROW(doc->cells, doc->cell_count, doc->cell_cap, 64);
            TableCell *tc = &doc->cells[doc->cell_count];
            SDL_zerop(tc);
            tc->row = row;
            tc->col = col++;
            uint32_t s = ts_node_start_byte(cell), e = ts_node_end_byte(cell);
            while (s < e && (doc->source[s] == ' ' || doc->source[s] == '\t')) s++;
            while (e > s && (doc->source[e - 1] == ' ' || doc->source[e - 1] == '\t')) e--;
            tc->text = str_add(doc, doc->source + s, e - s, &tc->text_len);
            add_pending(bd, -1, doc->cell_count, tc->text, tc->text_len);
            doc->cell_count++;
        }
        if (col > max_cols) max_cols = col;
        row++;
    }
    doc->blocks[bi].rows = row;
    doc->blocks[bi].cols = max_cols;
    doc->blocks[bi].cell_count = doc->cell_count - doc->blocks[bi].cell_first;
}

/* ------------------------------------------------------ front matter -- */

/* YAML front matter (--- ... --- at the top of the file) is shown as a
 * two-column table: one row per top-level key. Only the common subset of
 * YAML is understood, which is what front matter uses in practice:
 *   key: value            plain, 'single' or "double" quoted scalars
 *   key: [a, b]           flow lists          -> a, b (each as a code pill)
 *   key:\n  - a\n  - b     block lists         -> a, b (each as a code pill)
 *   key: | or >           block scalars       -> the lines joined
 *   key:\n  sub: x        nested maps         -> sub: x, ...
 * Comments and blank lines are skipped. Front matter with no key: value
 * line at all is shown as a YAML code block instead. */

typedef struct FmLine { const char *s; uint32_t len; int indent; } FmLine;

static const char *fm_trim(const char *s, uint32_t *len)
{
    while (*len && (*s == ' ' || *s == '\t')) { s++; (*len)--; }
    while (*len && (s[*len - 1] == ' ' || s[*len - 1] == '\t' || s[*len - 1] == '\r')) (*len)--;
    return s;
}

/* Strips a trailing " # comment" outside quotes. */
static void fm_strip_comment(const char *s, uint32_t *len)
{
    char quote = 0;
    for (uint32_t i = 0; i < *len; ++i) {
        char c = s[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) { *len = i; break; }
    }
    while (*len && (s[*len - 1] == ' ' || s[*len - 1] == '\t')) (*len)--;
}

/* A scalar without its quotes; double-quoted escapes \" \\ \n \t. */
static void fm_scalar(Document *doc, const char *s, uint32_t len)
{
    s = fm_trim(s, &len);
    if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
        for (uint32_t i = 1; i + 1 < len; ++i) {
            str_append(doc, s + i, 1);
            if (s[i] == '\'' && s[i + 1] == '\'') i++; /* '' -> ' */
        }
    } else if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        for (uint32_t i = 1; i + 1 < len; ++i) {
            if (s[i] == '\\' && i + 2 < len) {
                char e = s[++i];
                str_append(doc, e == 'n' ? " " : e == 't' ? " " : &s[i], 1);
            } else {
                str_append(doc, s + i, 1);
            }
        }
    } else {
        str_append(doc, s, len);
    }
}

static void fm_pill(Document *doc, uint32_t cell_text, const char *s, uint32_t len, bool *first)
{
    if (!*first) str_append(doc, ", ", 2);
    *first = false;
    uint32_t start = (uint32_t)doc->strings_len - cell_text;
    fm_scalar(doc, s, len);
    uint32_t end = (uint32_t)doc->strings_len - cell_text;
    if (end == start) return;
    GROW(doc->spans, doc->span_count, doc->span_cap, 256);
    Span *sp = &doc->spans[doc->span_count++];
    SDL_zerop(sp);
    sp->start = start;
    sp->len = end - start;
    sp->style = ST_CODE;
}

static bool fm_blank(const FmLine *l)
{
    uint32_t len = l->len;
    fm_trim(l->s, &len);
    return len == 0;
}

static void fm_cell(Document *doc, uint16_t row, uint16_t col, uint32_t text, uint32_t len,
                    uint32_t span_first)
{
    GROW(doc->cells, doc->cell_count, doc->cell_cap, 64);
    TableCell *tc = &doc->cells[doc->cell_count++];
    SDL_zerop(tc);
    tc->row = row;
    tc->col = col;
    tc->text = text;
    tc->text_len = len;
    tc->span_first = span_first;
    tc->span_count = doc->span_count - span_first;
}

static void walk_front_matter(Builder *bd, TSNode n, Ctx *ctx)
{
    Document *doc = bd->doc;
    uint32_t a = ts_node_start_byte(n), e = ts_node_end_byte(n);

    /* Split into lines, dropping the --- fences. */
    FmLine *lines = NULL;
    uint32_t count = 0, cap = 0;
    for (uint32_t p = a; p < e;) {
        uint32_t q = p;
        while (q < e && doc->source[q] != '\n') q++;
        uint32_t len = q - p;
        const char *s = doc->source + p;
        uint32_t tl = len;
        const char *t = fm_trim(s, &tl);
        bool fence = (tl == 3 && (SDL_memcmp(t, "---", 3) == 0 || SDL_memcmp(t, "...", 3) == 0));
        if (!fence) {
            GROW(lines, count, cap, 32);
            int indent = 0;
            while (indent < (int)len && (s[indent] == ' ' || s[indent] == '\t')) indent++;
            lines[count++] = (FmLine){s, len, indent};
        }
        p = q + 1;
    }

    /* Does it have any "key:" line? Otherwise show it verbatim. */
    bool any = false;
    for (uint32_t i = 0; i < count && !any; ++i) {
        uint32_t len = lines[i].len;
        const char *t = fm_trim(lines[i].s, &len);
        if (lines[i].indent == 0 && len && t[0] != '#' && t[0] != '-') {
            for (uint32_t k = 0; k < len; ++k) if (t[k] == ':' && (k + 1 == len || t[k + 1] == ' ')) { any = true; break; }
        }
    }
    if (!any) {
        push_block(bd, BLOCK_CODE, n, ctx);
        Block *b = &doc->blocks[doc->block_count - 1];
        uint32_t begin = str_begin(doc);
        for (uint32_t i = 0; i < count; ++i) {
            str_append(doc, lines[i].s, lines[i].len);
            if (i + 1 < count) str_append(doc, "\n", 1);
        }
        b->text = begin;
        b->text_len = str_end(doc, begin);
        b->lang = str_add(doc, "yaml", 4, &b->lang_len);
        SDL_free(lines);
        return;
    }

    push_block(bd, BLOCK_TABLE, n, ctx);
    int bi = doc->block_count - 1;
    doc->blocks[bi].cell_first = doc->cell_count;
    uint16_t row = 0;
    uint32_t sf = doc->span_count, tl;
    uint32_t t0 = str_add(doc, "Field", 5, &tl);
    fm_cell(doc, row, 0, t0, tl, sf);
    t0 = str_add(doc, "Value", 5, &tl);
    fm_cell(doc, row, 1, t0, tl, sf);
    row++;

    for (uint32_t i = 0; i < count;) {
        uint32_t len = lines[i].len;
        const char *t = fm_trim(lines[i].s, &len);
        if (!len || t[0] == '#' || lines[i].indent > 0) { i++; continue; }
        uint32_t colon = 0;
        while (colon < len && !(t[colon] == ':' && (colon + 1 == len || t[colon + 1] == ' '))) colon++;
        if (colon == len) { i++; continue; } /* not a key line */

        /* Key */
        uint32_t ks = str_begin(doc);
        sf = doc->span_count;
        fm_scalar(doc, t, colon);
        uint32_t kl = str_end(doc, ks);
        fm_cell(doc, row, 0, ks, kl, sf);

        /* Value */
        const char *v = t + colon + 1;
        uint32_t vl = len - colon - 1;
        fm_strip_comment(v, &vl);
        v = fm_trim(v, &vl);
        uint32_t vs = str_begin(doc);
        sf = doc->span_count;
        i++;
        if (vl >= 2 && v[0] == '[' && v[vl - 1] == ']') { /* flow list */
            bool first = true;
            uint32_t p = 1;
            while (p < vl - 1) {
                uint32_t q = p;
                char quote = 0;
                while (q < vl - 1 && (quote || v[q] != ',')) {
                    if (quote && v[q] == quote) quote = 0;
                    else if (!quote && (v[q] == '\'' || v[q] == '"')) quote = v[q];
                    q++;
                }
                uint32_t il = q - p;
                const char *it = fm_trim(v + p, &il);
                if (il) fm_pill(doc, vs, it, il, &first);
                p = q + 1;
            }
        } else if (vl && (v[0] == '|' || v[0] == '>')) { /* block scalar */
            bool first = true;
            while (i < count && (lines[i].indent > 0 || fm_blank(&lines[i]))) {
                uint32_t ll = lines[i].len;
                const char *l = fm_trim(lines[i].s, &ll);
                if (ll) {
                    if (!first) str_append(doc, " ", 1);
                    str_append(doc, l, ll);
                    first = false;
                }
                i++;
            }
        } else if (vl) {
            fm_scalar(doc, v, vl);
        } else { /* block list or nested map on the following lines */
            bool first = true;
            while (i < count) {
                uint32_t ll = lines[i].len;
                const char *l = fm_trim(lines[i].s, &ll);
                if (ll && lines[i].indent == 0 && l[0] != '-') break;
                i++;
                if (!ll || l[0] == '#') continue;
                fm_strip_comment(l, &ll);
                if (l[0] == '-' && (ll == 1 || l[1] == ' ')) {
                    uint32_t il = ll - 1;
                    const char *it = fm_trim(l + 1, &il);
                    fm_pill(doc, vs, it, il, &first);
                } else {
                    if (!first) str_append(doc, ", ", 2);
                    first = false;
                    str_append(doc, l, ll);
                }
            }
        }
        uint32_t vlen = str_end(doc, vs);
        fm_cell(doc, row, 1, vs, vlen, sf);
        row++;
    }
    SDL_free(lines);
    doc->blocks[bi].rows = row;
    doc->blocks[bi].cols = 2;
    doc->blocks[bi].cell_count = doc->cell_count - doc->blocks[bi].cell_first;
}

static void walk(Builder *bd, TSNode n, Ctx *ctx)
{
    Document *doc = bd->doc;
    if (is(n, "document") || is(n, "section")) {
        walk_children(bd, n, ctx);
    } else if (is(n, "block_quote")) {
        Ctx inner = *ctx;
        inner.quote_depth++;
        walk_children(bd, n, &inner);
        ctx->marker = inner.marker;
        ctx->task = inner.task;
    } else if (is(n, "list")) {
        Ctx inner = *ctx;
        inner.list_depth++;
        inner.marker = NULL;
        uint32_t count = ts_node_named_child_count(n);
        for (uint32_t i = 0; i < count; ++i) {
            TSNode c = ts_node_named_child(n, i);
            if (is(c, "list_item")) walk_list_item(bd, c, &inner);
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
        add_inline_block(bd, BLOCK_HEADING, n, child_of_type(n, "inline"), ctx);
        doc->blocks[doc->block_count - 1].level = level;
    } else if (is(n, "setext_heading")) {
        uint8_t level = ts_node_is_null(child_of_type(n, "setext_h2_underline")) ? 1 : 2;
        TSNode para = child_of_type(n, "paragraph");
        TSNode content = ts_node_is_null(para) ? para : child_of_type(para, "inline");
        add_inline_block(bd, BLOCK_HEADING, n, content, ctx);
        doc->blocks[doc->block_count - 1].level = level;
    } else if (is(n, "paragraph")) {
        TSNode content = child_of_type(n, "inline");
        add_inline_block(bd, BLOCK_PARAGRAPH, n, ts_node_is_null(content) ? n : content, ctx);
    } else if (is(n, "fenced_code_block")) {
        add_raw_block(bd, BLOCK_CODE, n, child_of_type(n, "code_fence_content"), ctx);
        TSNode info = child_of_type(n, "info_string");
        if (!ts_node_is_null(info)) {
            TSNode lang = child_of_type(info, "language");
            TSNode src = ts_node_is_null(lang) ? info : lang;
            uint32_t off = str_begin(doc);
            append_text(doc, src, false);
            uint32_t len = str_end(doc, off);
            doc->blocks[doc->block_count - 1].lang = off;
            doc->blocks[doc->block_count - 1].lang_len = len;
        }
    } else if (is(n, "indented_code_block")) {
        add_raw_block(bd, BLOCK_CODE, n, n, ctx);
        /* The node starts at the line start; drop the first line's indent
         * (continuation lines already lost theirs). */
        Block *b = &doc->blocks[doc->block_count - 1];
        uint32_t k = 0;
        while (k < 4 && k < b->text_len && doc->strings[b->text + k] == ' ') k++;
        b->text += k;
        b->text_len -= k;
    } else if (is(n, "thematic_break")) {
        push_block(bd, BLOCK_RULE, n, ctx);
    } else if (is(n, "pipe_table")) {
        walk_table(bd, n, ctx);
    } else if (is(n, "html_block")) {
        add_raw_block(bd, BLOCK_HTML, n, n, ctx);
    } else if (is(n, "link_reference_definition")) {
        add_link_def(bd, n);
    } else if (is(n, "minus_metadata")) {
        walk_front_matter(bd, n, ctx);
    } else if (is(n, "plus_metadata") || is(n, "block_continuation")) {
        /* not rendered */
    } else {
        walk_children(bd, n, ctx);
    }
}

/* ------------------------------------------------------------- inline -- */

/* Display text is built in a scratch buffer (urls of links are appended
 * to the document strings meanwhile) and copied out in one piece. */
typedef struct Inl {
    Builder *bd;
    const char *raw;      /* copy of the raw inline text being parsed */
    char *buf;
    size_t len, cap;
    uint32_t span_first;
} Inl;

static void out_append(Inl *in, const char *s, size_t n)
{
    if (in->len + n > in->cap) {
        size_t cap = in->cap ? in->cap * 2 : 256;
        while (cap < in->len + n) cap *= 2;
        in->buf = SDL_realloc(in->buf, cap);
        in->cap = cap;
    }
    SDL_memcpy(in->buf + in->len, s, n);
    in->len += n;
}

static void push_span(Inl *in, uint32_t start, uint32_t len, uint16_t style, uint32_t url, uint32_t url_len)
{
    Document *doc = in->bd->doc;
    if (len == 0) return;
    if (doc->span_count > in->span_first) {
        Span *last = &doc->spans[doc->span_count - 1];
        if (last->style == style && last->url == url && last->url_len == url_len &&
            last->start + last->len == start && !(style & ST_IMAGE)) {
            last->len += len;
            return;
        }
    }
    GROW(doc->spans, doc->span_count, doc->span_cap, 256);
    Span *s = &doc->spans[doc->span_count++];
    SDL_zerop(s);
    s->start = start;
    s->len = len;
    s->style = style;
    s->url = url;
    s->url_len = url_len;
}

/* Appends display text with the given style; soft line breaks -> space. */
static void emit(Inl *in, const char *s, size_t n, uint16_t style, uint32_t url, uint32_t url_len)
{
    uint32_t start = (uint32_t)in->len;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '\n') {
            /* drop trailing spaces before a soft break */
            while (in->len > start && in->buf[in->len - 1] == ' ') in->len--;
            c = ' ';
        }
        out_append(in, &c, 1);
    }
    uint32_t end = (uint32_t)in->len;
    if (end > start) push_span(in, start, end - start, style, url, url_len);
}

static void emit_utf8(Inl *in, uint32_t cp, uint16_t style, uint32_t url, uint32_t url_len)
{
    char buf[4];
    size_t n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    emit(in, buf, n, style, url, url_len);
}

static const struct { const char *name; uint32_t cp; } entities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"laquo", 0xAB},
    {"raquo", 0xBB}, {"bull", 0x2022}, {"middot", 0xB7}, {"times", 0xD7},
    {"rarr", 0x2192}, {"larr", 0x2190}, {"deg", 0xB0}, {"euro", 0x20AC},
};

static bool lookup_def(Builder *bd, const char *label, size_t n, uint32_t *url, uint32_t *url_len)
{
    Document *doc = bd->doc;
    size_t mark = doc->strings_len;
    uint32_t len;
    uint32_t off = normalise_label(doc, label, n, &len);
    bool found = false;
    for (uint32_t i = 0; i < bd->def_count; ++i) {
        LinkDef *d = &bd->defs[i];
        if (d->label_len == len && SDL_memcmp(doc->strings + d->label, doc->strings + off, len) == 0) {
            *url = d->url;
            *url_len = d->url_len;
            found = true;
            break;
        }
    }
    doc->strings_len = mark; /* discard the temporary */
    return found;
}

static void emit_node(Inl *in, TSNode n, uint16_t style, uint32_t url, uint32_t url_len);

static void emit_children(Inl *in, TSNode n, uint16_t style, uint32_t url, uint32_t url_len)
{
    uint32_t pos = ts_node_start_byte(n), end = ts_node_end_byte(n);
    uint32_t count = ts_node_child_count(n);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode c = ts_node_child(n, i);
        uint32_t cs = ts_node_start_byte(c);
        if (cs > pos) emit(in, in->raw + pos, cs - pos, style, url, url_len);
        emit_node(in, c, style, url, url_len);
        pos = ts_node_end_byte(c);
    }
    if (end > pos) emit(in, in->raw + pos, end - pos, style, url, url_len);
}

static void node_range(TSNode n, uint32_t *s, uint32_t *e)
{
    *s = ts_node_start_byte(n);
    *e = ts_node_end_byte(n);
}

static void emit_node(Inl *in, TSNode n, uint16_t style, uint32_t url, uint32_t url_len)
{
    Document *doc = in->bd->doc;
    const char *t = ts_node_type(n);
    uint32_t s, e;
    node_range(n, &s, &e);

    if (!ts_node_is_named(n)) {
        emit(in, in->raw + s, e - s, style, url, url_len);
    } else if (is(n, "emphasis_delimiter") || is(n, "code_span_delimiter") || is(n, "html_tag") ||
               is(n, "latex_span_delimiter")) {
        /* markup: hidden */
    } else if (is(n, "emphasis")) {
        emit_children(in, n, style | ST_ITALIC, url, url_len);
    } else if (is(n, "strong_emphasis")) {
        emit_children(in, n, style | ST_BOLD, url, url_len);
    } else if (is(n, "strikethrough")) {
        emit_children(in, n, style | ST_STRIKE, url, url_len);
    } else if (is(n, "code_span")) {
        uint32_t count = ts_node_child_count(n);
        if (count >= 2) {
            s = ts_node_end_byte(ts_node_child(n, 0));
            e = ts_node_start_byte(ts_node_child(n, count - 1));
        }
        /* One leading and trailing space is stripped when both exist. */
        if (e - s >= 2 && in->raw[s] == ' ' && in->raw[e - 1] == ' ') { s++; e--; }
        emit(in, in->raw + s, e > s ? e - s : 0, style | ST_CODE, url, url_len);
    } else if (is(n, "inline_link") || is(n, "full_reference_link") ||
               is(n, "collapsed_reference_link") || is(n, "shortcut_link")) {
        TSNode text = child_of_type(n, "link_text");
        uint32_t u = 0, ul = 0;
        bool ok = true;
        if (is(n, "inline_link")) {
            TSNode dest = child_of_type(n, "link_destination");
            if (!ts_node_is_null(dest)) {
                uint32_t ds, de;
                node_range(dest, &ds, &de);
                if (de > ds && in->raw[ds] == '<') { ds++; if (de > ds && in->raw[de - 1] == '>') de--; }
                u = str_add(doc, in->raw + ds, de - ds, &ul);
            }
        } else {
            TSNode label = is(n, "full_reference_link") ? child_of_type(n, "link_label") : text;
            uint32_t ls = 0, le = 0;
            if (!ts_node_is_null(label)) node_range(label, &ls, &le);
            ok = lookup_def(in->bd, in->raw + ls, le - ls, &u, &ul);
        }
        if (!ok) { /* unresolved reference: literal text */
            emit_children(in, n, style, url, url_len);
            return;
        }
        if (!ts_node_is_null(text)) emit_children(in, text, style | ST_LINK, u, ul);
    } else if (is(n, "image")) {
        TSNode desc = child_of_type(n, "image_description");
        TSNode dest = child_of_type(n, "link_destination");
        uint32_t u = 0, ul = 0;
        if (!ts_node_is_null(dest)) {
            uint32_t ds, de;
            node_range(dest, &ds, &de);
            if (de > ds && in->raw[ds] == '<') { ds++; if (de > ds && in->raw[de - 1] == '>') de--; }
            u = str_add(doc, in->raw + ds, de - ds, &ul);
        }
        uint32_t as = 0, ae = 0;
        if (!ts_node_is_null(desc)) {
            node_range(desc, &as, &ae);
            if (ae > as && in->raw[as] == '[') as++;
            if (ae > as && in->raw[ae - 1] == ']') ae--;
        }
        /* Always at least one character so the image has a span. */
        if (ae > as) emit(in, in->raw + as, ae - as, (uint16_t)(style | ST_IMAGE), u, ul);
        else emit(in, " ", 1, (uint16_t)(style | ST_IMAGE), u, ul);
    } else if (is(n, "uri_autolink") || is(n, "email_autolink")) {
        if (e - s >= 2) { s++; e--; } /* strip < > */
        uint32_t ul;
        uint32_t u;
        if (is(n, "email_autolink")) {
            uint32_t off = str_begin(doc);
            str_append(doc, "mailto:", 7);
            str_append(doc, in->raw + s, e - s);
            ul = str_end(doc, off);
            u = off;
        } else {
            u = str_add(doc, in->raw + s, e - s, &ul);
        }
        emit(in, in->raw + s, e - s, style | ST_LINK, u, ul);
    } else if (is(n, "backslash_escape")) {
        if (e - s >= 2) emit(in, in->raw + s + 1, e - s - 1, style, url, url_len);
    } else if (is(n, "hard_line_break")) {
        while (in->len > 0 && in->buf[in->len - 1] == ' ') in->len--;
        uint32_t start = (uint32_t)in->len;
        out_append(in, "\n", 1);
        push_span(in, start, 1, style, url, url_len);
    } else if (is(n, "entity_reference")) {
        const char *name = in->raw + s + 1;
        size_t nl = e - s >= 2 ? e - s - 2 : 0;
        for (size_t i = 0; i < SDL_arraysize(entities); ++i) {
            if (SDL_strlen(entities[i].name) == nl && SDL_memcmp(entities[i].name, name, nl) == 0) {
                emit_utf8(in, entities[i].cp, style, url, url_len);
                return;
            }
        }
        emit(in, in->raw + s, e - s, style, url, url_len);
    } else if (is(n, "numeric_character_reference")) {
        const char *p = in->raw + s + 2; /* after &# */
        uint32_t cp = 0;
        bool hex = *p == 'x' || *p == 'X';
        if (hex) p++;
        for (; p < in->raw + e && *p != ';'; ++p) {
            char c = *p;
            if (hex) cp = cp * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            else cp = cp * 10 + (uint32_t)(c - '0');
        }
        emit_utf8(in, cp && cp < 0x110000 ? cp : 0xFFFD, style, url, url_len);
    } else if (ts_node_child_count(n) > 0 && !is(n, "latex_block")) {
        (void)t;
        emit_children(in, n, style, url, url_len);
    } else if (is(n, "latex_block")) {
        emit(in, in->raw + s, e - s, style | ST_CODE, url, url_len);
    } else {
        emit(in, in->raw + s, e - s, style, url, url_len);
    }
}

/* Parses one inline text; produces display text + spans. */
static void process_inline(Builder *bd, TSParser *parser, uint32_t raw, uint32_t raw_len,
                           uint32_t *text, uint32_t *text_len, uint32_t *span_first, uint32_t *span_count)
{
    Document *doc = bd->doc;
    /* Copy: strings may be reallocated while emitting. */
    char *copy = SDL_malloc(raw_len + 1);
    SDL_memcpy(copy, doc->strings + raw, raw_len);
    copy[raw_len] = 0;

    Inl in = {bd, copy, NULL, 0, 0, doc->span_count};
    TSTree *tree = ts_parser_parse_string(parser, NULL, copy, raw_len);
    if (tree) {
        emit_children(&in, ts_tree_root_node(tree), 0, 0, 0);
        ts_tree_delete(tree);
    } else {
        emit(&in, copy, raw_len, 0, 0, 0);
    }
    SDL_free(copy);

    /* Trim surrounding whitespace, shifting spans for the leading part. */
    size_t lead = 0;
    while (lead < in.len && (in.buf[lead] == ' ' || in.buf[lead] == '\n')) lead++;
    while (in.len > lead && (in.buf[in.len - 1] == ' ' || in.buf[in.len - 1] == '\n')) in.len--;
    uint32_t len = (uint32_t)(in.len - lead);
    *text = str_add(doc, in.buf ? in.buf + lead : "", len, text_len);
    SDL_free(in.buf);
    *span_first = in.span_first;
    *span_count = doc->span_count - in.span_first;
    for (uint32_t i = in.span_first; i < doc->span_count; ++i) {
        Span *sp = &doc->spans[i];
        uint32_t s0 = sp->start, e0 = sp->start + sp->len;
        s0 = s0 > lead ? s0 - (uint32_t)lead : 0;
        e0 = e0 > lead ? e0 - (uint32_t)lead : 0;
        if (e0 > len) e0 = len;
        if (s0 > e0) s0 = e0;
        sp->start = s0;
        sp->len = e0 - s0;
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
    if (doc->source_len > UINT32_MAX / 4) {
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
    if (!tree) {
        ts_parser_delete(parser);
        SDL_asprintf(&doc->error, "Cannot parse file");
        return doc;
    }

    Builder bd = {doc, NULL, 0, 0, NULL, 0, 0};
    Ctx ctx = {0};
    walk(&bd, ts_tree_root_node(tree), &ctx);
    ts_tree_delete(tree);

    /* Inline pass, now that all link reference definitions are known. */
    ts_parser_set_language(parser, tree_sitter_markdown_inline());
    for (uint32_t i = 0; i < bd.pending_count; ++i) {
        Pending *p = &bd.pending[i];
        uint32_t text, len, sf, sc;
        process_inline(&bd, parser, p->raw, p->raw_len, &text, &len, &sf, &sc);
        if (p->block >= 0) {
            Block *b = &doc->blocks[p->block];
            b->text = text;
            b->text_len = len;
            b->span_first = sf;
            b->span_count = sc;
        } else {
            TableCell *c = &doc->cells[p->cell];
            c->text = text;
            c->text_len = len;
            c->span_first = sf;
            c->span_count = sc;
        }
    }
    ts_parser_delete(parser);

    /* Syntax highlighting of fenced code (spans with colour classes). */
    for (int i = 0; i < doc->block_count; ++i) {
        Block *b = &doc->blocks[i];
        if (b->kind != BLOCK_CODE || !b->lang_len) continue;
        b->span_first = doc->span_count;
        highlight_code(document_str(doc, b->lang), b->lang_len,
                       document_str(doc, b->text), b->text_len,
                       &doc->spans, &doc->span_count, &doc->span_cap);
        b->span_count = doc->span_count - b->span_first;
    }

    SDL_free(bd.pending);
    SDL_free(bd.defs);
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
    SDL_free(doc->spans);
    SDL_free(doc->cells);
    SDL_free(doc->error);
    SDL_free(doc);
}
