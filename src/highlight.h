#ifndef MD_HIGHLIGHT_H
#define MD_HIGHLIGHT_H

#include <stddef.h>
#include <stdint.h>
#include "document.h"

/* Syntax classes used for code block colouring: one per colour role in
 * Catppuccin's Neovim tree-sitter groups (lua/catppuccin/groups/treesitter.lua). */
enum {
    HL_NONE,              /* plain text, @variable */
    HL_KEYWORD,           /* mauve: @keyword.*, @type.builtin, @constant.macro */
    HL_STRING,            /* green */
    HL_COMMENT,           /* overlay2 */
    HL_NUMBER,            /* peach: numbers, booleans */
    HL_CONSTANT,          /* peach: @constant, @constant.builtin, @attribute */
    HL_FUNCTION,          /* blue: functions, methods, @string.special.url */
    HL_TYPE,              /* yellow: @type, @module, @constructor */
    HL_VARIABLE_BUILTIN,  /* red */
    HL_OPERATOR,          /* sky */
    HL_PUNCTUATION,       /* overlay2: brackets, delimiters */
    HL_PROPERTY,          /* lavender: @property, @variable.member */
    HL_TAG,               /* blue */
    HL_ATTRIBUTE,         /* yellow: @tag.attribute */
    HL_PARAMETER,         /* maroon: @variable.parameter */
    HL_FUNCTION_BUILTIN,  /* peach */
    HL_SPECIAL,           /* pink: escapes, regexps, macros, preprocessor, @punctuation.special */
    HL_CHARACTER,         /* teal: @character, @string.documentation, @tag.delimiter */
    HL_LABEL,             /* sapphire */
    HL_SYMBOL,            /* flamingo: @string.special.symbol */
    HL_COUNT
};

/* Appends colour spans (Span.color = HL_*) for `code` written in `lang`
 * (a fenced code block info string such as "c" or "python"). Does nothing
 * for unknown languages. Thread-safe. */
void highlight_code(const char *lang, size_t lang_len, const char *code, size_t len,
                    Span **spans, uint32_t *count, uint32_t *cap);

#endif
