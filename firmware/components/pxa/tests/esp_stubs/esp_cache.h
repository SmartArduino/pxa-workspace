#ifndef ESP_CACHE_H
#define ESP_CACHE_H

#include <stddef.h>

#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1

static inline int esp_cache_msync(void *address, size_t size, int flags) {
    (void)address;
    (void)size;
    (void)flags;
    return 0;
}

#endif
