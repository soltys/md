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

typedef struct Block {
    BlockKind kind;
    uint8_t level;        /* heading level 1-6 */
    uint8_t list_depth;   /* nesting of lists around this block */
    uint8_t quote_depth;  /* nesting of block quotes */
    uint8_t task;         /* TASK_* for the first block of a task item */
    uint32_t text;        /* offset of display text in Document.strings */
    uint32_t text_len;
    uint32_t marker;      /* list marker ("•", "1.") in strings; 0 len = none */
    uint32_t marker_len;
    uint32_t lang;        /* fenced code info string */
    uint32_t lang_len;
    uint32_t src_start;   /* byte range in the source, for scroll sync later */
    uint32_t src_end;
    uint32_t src_line;    /* 1-based first source line */
} Block;

typedef struct Document {
    char *path;
    char *source;
    size_t source_len;
    char *strings;        /* display text for all blocks, NUL-separated */
    size_t strings_len;
    size_t strings_cap;
    Block *blocks;
    int block_count;
    int block_cap;
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

#endif
