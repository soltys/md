#ifndef MD_DOCUMENT_H
#define MD_DOCUMENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum BlockKind {
    BLOCK_PARAGRAPH,
    BLOCK_HEADING,
    BLOCK_CODE,
    BLOCK_RULE,
    BLOCK_TABLE,
    BLOCK_HTML,
} BlockKind;

enum { TASK_NONE, TASK_UNCHECKED, TASK_CHECKED };

/* Inline style bits. */
enum {
    ST_BOLD   = 1 << 0,
    ST_ITALIC = 1 << 1,
    ST_CODE   = 1 << 2,
    ST_STRIKE = 1 << 3,
    ST_LINK   = 1 << 4,
    ST_IMAGE  = 1 << 5,   /* text is the alt text, url the image source */
};

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* A styled range of a block's (or table cell's) display text. Offsets are
 * relative to the start of that text. */
typedef struct Span {
    uint32_t start, len;
    uint16_t style;
    uint8_t color;        /* syntax highlight class (code blocks), 0 = none */
    uint8_t pad;
    uint32_t url, url_len; /* links and images, offset into Document.strings */
} Span;

typedef struct TableCell {
    uint32_t text, text_len;
    uint32_t span_first, span_count;
    uint16_t row, col;
} TableCell;

typedef struct Block {
    BlockKind kind;
    uint8_t level;        /* heading level 1-6 */
    uint8_t list_depth;   /* nesting of lists around this block */
    uint8_t quote_depth;  /* nesting of block quotes */
    uint8_t task;         /* TASK_* for the first block of a task item */
    uint32_t text;        /* display text offset in Document.strings */
    uint32_t text_len;
    uint32_t span_first, span_count;
    uint32_t marker;      /* list marker ("•", "1.") in strings; 0 len = none */
    uint32_t marker_len;
    uint32_t lang;        /* fenced code info string */
    uint32_t lang_len;
    uint32_t cell_first, cell_count; /* tables */
    uint16_t rows, cols;
    uint32_t aligns;      /* tables: 2 bits per column, first 16 columns */
    uint32_t src_start;   /* byte range in the source */
    uint32_t src_end;
    uint32_t src_line;    /* 1-based first source line */
} Block;

typedef struct Document {
    char *path;
    char *source;
    size_t source_len;
    char *strings;        /* display text, NUL-separated */
    size_t strings_len, strings_cap;
    Block *blocks;
    int block_count, block_cap;
    Span *spans;
    uint32_t span_count, span_cap;
    TableCell *cells;
    uint32_t cell_count, cell_cap;
    char *error;          /* non-NULL when loading failed */
} Document;

/* Reads and parses a file. Thread-safe, intended for a worker thread.
 * Always returns a document; check ->error. */
Document *document_load(const char *path);

/* Parses text already in memory; takes ownership of `text` (SDL_malloc). */
Document *document_parse(const char *path, char *text, size_t len);

void document_free(Document *doc);

static inline const char *document_str(const Document *doc, uint32_t off)
{
    return doc->strings + off;
}

static inline int table_align(const Block *b, int col)
{
    return col < 16 ? (int)((b->aligns >> (2 * col)) & 3) : ALIGN_LEFT;
}

#endif
