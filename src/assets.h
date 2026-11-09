#ifndef MD_ASSETS_H
#define MD_ASSETS_H

#include <stddef.h>
#include <stdbool.h>
#include "asset_ids.h"

/* Returns a read-only view of an embedded asset. On Windows the bytes live
 * inside the mapped executable image, so this is free and never copies.
 * The memory stays valid for the whole process lifetime. */
bool asset_get(int id, const void **data, size_t *size);

#endif
