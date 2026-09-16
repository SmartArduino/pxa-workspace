#ifndef PXA_ESP_UI_ASSETS_H
#define PXA_ESP_UI_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#include "pxa/package.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t current_bytes;
    size_t peak_bytes;
    size_t largest_decoded_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t evictions;
    uint32_t asset_count;
    uint32_t referenced_assets;
} pxa_esp_ui_asset_snapshot_t;

int pxa_esp_ui_assets_begin(const pxa_package_manifest_t *manifest,
                            const char *package_root);
void pxa_esp_ui_assets_clear(void);
void pxa_esp_ui_assets_snapshot(pxa_esp_ui_asset_snapshot_t *output);

const void *pxa_esp_ui_asset_resolve(const uint8_t *path, size_t path_size,
                                     void *user_data);
void pxa_esp_ui_asset_release(const void *source, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
