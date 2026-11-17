#ifndef MD_ASSETS_H
#define MD_ASSETS_H

#include <stddef.h>
#include <stdbool.h>
#include "asset_ids.h"

/* Returns a read-only view of an embedded asset; the memory stays valid for
 * the whole process lifetime. Uncompressed assets are read straight out of
 * the mapped executable (free, no copy). Compressed ones (the fonts, see
 * cmake/Fonts.cmake) are unpacked into memory on the first call, once; a
 * thread that asks while another one is unpacking waits for it.
 * Safe to call from any thread. */
bool asset_get(int id, const void **data, size_t *size);

/* Starts unpacking these assets on a background thread, so a later
 * asset_get() finds them ready. Returns at once; ids already unpacked or
 * being unpacked cost nothing. */
void asset_prefetch(const int *ids, int count);

#endif
