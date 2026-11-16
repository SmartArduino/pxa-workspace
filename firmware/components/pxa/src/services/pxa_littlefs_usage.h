/* SPDX-License-Identifier: MIT */
#ifndef PXA_LITTLEFS_USAGE_H
#define PXA_LITTLEFS_USAGE_H

#include "lfs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *seen;
    lfs_block_t capacity;
    lfs_size_t used;
} pxa_littlefs_usage_t;

static int pxa_littlefs_usage_visit(void *context, lfs_block_t block) {
    pxa_littlefs_usage_t *usage = context;
    if (block >= usage->capacity) return LFS_ERR_CORRUPT;
    const uint8_t mask = (uint8_t)(1u << (block & 7u));
    if (!(usage->seen[block / 8u] & mask)) {
        usage->seen[block / 8u] |= mask;
        usage->used++;
    }
    return 0;
}

/* Traverse under the driver's filesystem semaphore. Deduplicate shared CTZ
 * chains, while retaining distinct blocks referenced by open old versions. */
static int pxa_littlefs_usage_count(lfs_t *fs,
                                  void *(*allocate)(size_t, size_t),
                                  void (*release)(void *),
                                  lfs_size_t *used) {
    if (!fs || !fs->block_count || !allocate || !release || !used)
        return LFS_ERR_INVAL;
    const size_t bytes = (size_t)(fs->block_count / 8u) +
                         ((fs->block_count & 7u) != 0u);
    pxa_littlefs_usage_t usage = {
        .seen = allocate(bytes, 1u), .capacity = fs->block_count, .used = 0u
    };
    if (!usage.seen) return LFS_ERR_NOMEM;
    const int result = lfs_fs_traverse(fs, pxa_littlefs_usage_visit, &usage);
    release(usage.seen);
    if (result == 0) *used = usage.used;
    return result;
}

#endif
