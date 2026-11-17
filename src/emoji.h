#ifndef MD_EMOJI_H
#define MD_EMOJI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Decodes one UTF-8 code point; returns its byte length (>= 1). */
size_t utf8_decode(const char *s, size_t n, uint32_t *cp);

/* If an emoji (including ZWJ sequences, skin tones, flags, keycaps and
 * VS16 presentation) starts at s, returns its length in bytes, else 0.
 * Such clusters are drawn with the emoji font directly so HarfBuzz shapes
 * the whole sequence in one font. */
size_t emoji_cluster(const char *s, size_t n);

/* True if the text contains any emoji (a quick scan; the loader uses it to
 * start unpacking the emoji font early). */
bool text_has_emoji(const char *s, size_t n);

#endif
