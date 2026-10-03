#include <stddef.h>

#include <esp_heap_caps.h>
#include <sdkconfig.h>

#if CONFIG_LV_USE_STDLIB_MALLOC == 255
/* Selected with CONFIG_LV_USE_STDLIB_MALLOC=255 (LV_STDLIB_CUSTOM). LVGL's
 * allocations are many and small (widgets, styles, event lists), and leaving
 * them in internal RAM starves the DMA-capable pool that the panel bounce
 * buffer and the audio path need. Routed to PSRAM instead: measured on the
 * SenseCAP Watcher, lv_init() plus the UI allocated ~66 KiB internally. */
void* lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* pointer, size_t new_size) {
    return heap_caps_realloc(pointer, new_size,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void* pointer) {
    heap_caps_free(pointer);
}

/* LVGL calls these once from lv_init()/lv_deinit() when the allocator is
 * external; there is no pool to set up because PSRAM is the pool. */
void lv_mem_init(void) {}

void lv_mem_deinit(void) {}
#endif
