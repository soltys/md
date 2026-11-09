#ifndef MD_REGEX_H
#define MD_REGEX_H

#include <stddef.h>
#include <stdbool.h>

/* Tiny regex matcher for tree-sitter query predicates (#match?).
 * Supports ^ $ . [] [^] ranges, \d \w \s escapes, * + ? quantifiers.
 * Returns -1 for unsupported patterns (groups, alternation), else 0/1. */
int regex_search(const char *pattern, const char *text, size_t len);

#endif
