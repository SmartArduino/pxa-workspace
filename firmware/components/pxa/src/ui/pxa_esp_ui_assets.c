#include "pxa_esp_ui_assets.h"

#if defined(ESP_PLATFORM)

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

typedef struct pxa_esp_ui_asset {
    struct pxa_esp_ui_asset *next;
    lv_image_dsc_t descriptor;
    uint8_t *bytes;
    char *path;
    size_t size;
    size_t path_size;
    size_t references;
    uint64_t last_used;
} pxa_esp_ui_asset_t;

typedef struct {
    const pxa_package_manifest_t *manifest;
    pxa_esp_ui_asset_t *assets;
    size_t current_bytes;
    size_t peak_bytes;
    size_t largest_decoded_bytes;
    uint64_t clock;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t evictions;
    char *package_root;
    size_t package_root_size;
} pxa_esp_ui_asset_cache_t;

static pxa_esp_ui_asset_cache_t g_assets;

static void *asset_alloc(size_t size) {
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return memory;
}

static int size_add(size_t left, size_t right, size_t *output) {
    if (left > SIZE_MAX - right) return 0;
    *output = left + right;
    return 1;
}

static uint32_t read_png_u32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

static void destroy_asset(pxa_esp_ui_asset_t *asset) {
    if (asset == NULL) return;
    if (asset->descriptor.header.magic == LV_IMAGE_HEADER_MAGIC) {
        lv_image_cache_drop(&asset->descriptor);
    }
    free(asset->path);
    free(asset->bytes);
    free(asset);
}

void pxa_esp_ui_assets_clear(void) {
    lv_lock();
    while (g_assets.assets != NULL) {
        pxa_esp_ui_asset_t *asset = g_assets.assets;
        g_assets.assets = asset->next;
        destroy_asset(asset);
    }
    free(g_assets.package_root);
    memset(&g_assets, 0, sizeof(g_assets));
    lv_unlock();
}

static int evict_one_unused_asset(void) {
    pxa_esp_ui_asset_t *asset;
    pxa_esp_ui_asset_t *previous = NULL;
    pxa_esp_ui_asset_t *victim = NULL;
    pxa_esp_ui_asset_t *victim_previous = NULL;
    for (asset = g_assets.assets; asset != NULL; asset = asset->next) {
        if (asset->references == 0 &&
            (victim == NULL || asset->last_used < victim->last_used)) {
            victim = asset;
            victim_previous = previous;
        }
        previous = asset;
    }
    if (victim == NULL) return 0;
    if (victim_previous == NULL) {
        g_assets.assets = victim->next;
    } else {
        victim_previous->next = victim->next;
    }
    g_assets.current_bytes -= victim->size;
    g_assets.evictions++;
    destroy_asset(victim);
    return 1;
}

static void *asset_alloc_reclaiming_unused(size_t size) {
    void *memory;
    if (size == 0) return NULL;
    while ((memory = asset_alloc(size)) == NULL) {
        if (!evict_one_unused_asset()) return NULL;
    }
    return memory;
}

int pxa_esp_ui_assets_begin(const pxa_package_manifest_t *manifest,
                            const char *package_root) {
    size_t root_size;
    char *root_copy;
    if (manifest == NULL || package_root == NULL) return 0;
    root_size = strlen(package_root);
    if (root_size == 0 || root_size == SIZE_MAX) return 0;
    lv_lock();
    root_copy = (char *)asset_alloc_reclaiming_unused(root_size + 1u);
    if (root_copy == NULL) {
        lv_unlock();
        return 0;
    }
    memcpy(root_copy, package_root, root_size + 1u);
    pxa_esp_ui_assets_clear();
    g_assets.package_root = root_copy;
    g_assets.package_root_size = root_size;
    g_assets.manifest = manifest;
    lv_unlock();
    return 1;
}

const void *pxa_esp_ui_asset_resolve(const uint8_t *path, size_t path_size,
                                     void *user_data) {
    static const uint8_t signature[] = {0x89, 0x50, 0x4e, 0x47,
                                        0x0d, 0x0a, 0x1a, 0x0a};
    const pxa_package_file_t *file;
    pxa_esp_ui_asset_t *asset;
    size_t decoded_bytes;
    size_t file_size;
    size_t full_path_size;
    uint32_t height;
    uint32_t width;
    lv_image_header_t decoded_header;
    char *full_path = NULL;
    struct stat metadata;
    FILE *stream = NULL;
    int locked = 1;
    (void)user_data;
    lv_lock();
    if (path == NULL || path_size == 0 || path_size == SIZE_MAX ||
        g_assets.manifest == NULL ||
        g_assets.package_root == NULL || g_assets.package_root_size == 0) {
        lv_unlock();
        return NULL;
    }
    file = pxa_package_file_find(g_assets.manifest,
                                 (pxa_bytes_t){path, path_size});
    if (file == NULL || file->size < 24 || file->size > SIZE_MAX) {
        lv_unlock();
        return NULL;
    }
    file_size = (size_t)file->size;
    for (asset = g_assets.assets; asset != NULL; asset = asset->next) {
        if (asset->size == file_size && asset->path_size == path_size &&
            memcmp(asset->path, path, path_size) == 0) {
            asset->references++;
            asset->last_used = ++g_assets.clock;
            g_assets.cache_hits++;
            lv_unlock();
            return &asset->descriptor;
        }
    }
    g_assets.cache_misses++;
    asset = (pxa_esp_ui_asset_t *)asset_alloc_reclaiming_unused(sizeof(*asset));
    if (asset == NULL) {
        lv_unlock();
        return NULL;
    }
    memset(asset, 0, sizeof(*asset));
    asset->bytes = (uint8_t *)asset_alloc_reclaiming_unused(file_size);
    if (asset->bytes == NULL) goto failed;
    if (!size_add(g_assets.package_root_size, path_size, &full_path_size) ||
        !size_add(full_path_size, 2u, &full_path_size)) {
        goto failed;
    }
    full_path = (char *)asset_alloc_reclaiming_unused(full_path_size);
    if (full_path == NULL) goto failed;
    memcpy(full_path, g_assets.package_root, g_assets.package_root_size);
    full_path[g_assets.package_root_size] = '/';
    memcpy(full_path + g_assets.package_root_size + 1u, path, path_size);
    full_path[full_path_size - 1u] = '\0';
    lv_unlock();
    locked = 0;
    if (stat(full_path, &metadata) != 0 || metadata.st_size < 0 ||
        (uint64_t)metadata.st_size != file_size) {
        goto failed;
    }
    stream = fopen(full_path, "rb");
    free(full_path);
    full_path = NULL;
    if (stream == NULL) goto failed;
    if (fread(asset->bytes, 1, file_size, stream) != file_size) goto failed;
    fclose(stream);
    stream = NULL;
    lv_lock();
    locked = 1;
    width = read_png_u32(asset->bytes + 16);
    height = read_png_u32(asset->bytes + 20);
    if (memcmp(asset->bytes, signature, sizeof(signature)) != 0 ||
        memcmp(asset->bytes + 12, "IHDR", 4) != 0 ||
        width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4u) {
        goto failed;
    }
    decoded_bytes = (size_t)width * 4u;
    if ((size_t)height > SIZE_MAX / decoded_bytes ||
        file_size > SIZE_MAX - g_assets.current_bytes) {
        goto failed;
    }
    decoded_bytes *= (size_t)height;
    asset->path = (char *)asset_alloc_reclaiming_unused(path_size + 1u);
    if (asset->path == NULL) goto failed;
    memcpy(asset->path, path, path_size);
    asset->path[path_size] = '\0';
    asset->path_size = path_size;
    asset->size = file_size;
    asset->references = 1;
    asset->last_used = ++g_assets.clock;
    asset->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    asset->descriptor.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    asset->descriptor.header.w = width;
    asset->descriptor.header.h = height;
    asset->descriptor.data_size = asset->size;
    asset->descriptor.data = asset->bytes;
    /* A PNG asset must be accepted by an active LVGL decoder before it can
     * reach the canvas renderer. RAW_ALPHA alone has no renderable bpp. */
    if (lv_image_decoder_get_info(&asset->descriptor, &decoded_header) !=
        LV_RESULT_OK) {
        goto failed;
    }
    asset->next = g_assets.assets;
    g_assets.assets = asset;
    g_assets.current_bytes += asset->size;
    if (g_assets.current_bytes > g_assets.peak_bytes) {
        g_assets.peak_bytes = g_assets.current_bytes;
    }
    if (decoded_bytes > g_assets.largest_decoded_bytes) {
        g_assets.largest_decoded_bytes = decoded_bytes;
    }
    lv_unlock();
    return &asset->descriptor;

failed:
    if (stream != NULL) fclose(stream);
    free(full_path);
    if (!locked) lv_lock();
    destroy_asset(asset);
    lv_unlock();
    return NULL;
}

void pxa_esp_ui_asset_release(const void *source, void *user_data) {
    pxa_esp_ui_asset_t *asset;
    (void)user_data;
    lv_lock();
    for (asset = g_assets.assets; asset != NULL; asset = asset->next) {
        if (&asset->descriptor != source) continue;
        if (asset->references != 0) asset->references--;
        asset->last_used = ++g_assets.clock;
        lv_unlock();
        return;
    }
    lv_unlock();
}

void pxa_esp_ui_assets_snapshot(pxa_esp_ui_asset_snapshot_t *output) {
    pxa_esp_ui_asset_t *asset;
    if (output == NULL) return;
    lv_lock();
    memset(output, 0, sizeof(*output));
    output->current_bytes = g_assets.current_bytes;
    output->peak_bytes = g_assets.peak_bytes;
    output->largest_decoded_bytes = g_assets.largest_decoded_bytes;
    output->cache_hits = g_assets.cache_hits;
    output->cache_misses = g_assets.cache_misses;
    output->evictions = g_assets.evictions;
    for (asset = g_assets.assets; asset != NULL; asset = asset->next) {
        output->asset_count++;
        if (asset->references != 0) output->referenced_assets++;
    }
    lv_unlock();
}

#endif
