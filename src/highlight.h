#ifndef MD_HIGHLIGHT_H
#define MD_HIGHLIGHT_H

#include <stddef.h>
#include <stdint.h>
#include "document.h"

/* Syntax classes used for code block colouring. */
enum {
    HL_NONE,
    HL_KEYWORD,
    HL_STRING,
    HL_COMMENT,
    HL_NUMBER,
    HL_CONSTANT,
    HL_FUNCTION,
    HL_TYPE,
    HL_VARIABLE_BUILTIN,
    HL_OPERATOR,
    HL_PUNCTUATION,
    HL_PROPERTY,
    HL_TAG,
    HL_ATTRIBUTE,
    HL_COUNT
};

/* Appends colour spans (Span.color = HL_*) for `code` written in `lang`
 * (a fenced code block info string such as "c" or "python"). Does nothing
 * for unknown languages. Thread-safe. */
void highlight_code(const char *lang, size_t lang_len, const char *code, size_t len,
                    Span **spans, uint32_t *count, uint32_t *cap);

#endif
