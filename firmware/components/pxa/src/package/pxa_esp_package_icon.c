#include "pxa_esp_package_icon.h"

#if defined(ESP_PLATFORM)

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include "pxa_esp_package_store.h"

#define PXA_ESP_PACKAGE_MAX_ICON_BYTES (64u * 1024u)
#define PXA_ESP_PACKAGE_MAX_ICON_DIMENSION 256u
#define PXA_ESP_PACKAGE_ASSET_PATH_BYTES \
    (PXA_ESP_PACKAGE_ROOT_BYTES + PXA_ESP_PACKAGE_ICON_PATH_BYTES)

typedef struct {
    lv_image_dsc_t descriptor;
    uint8_t bytes[];
} pxa_esp_package_icon_resource_t;

static void *icon_alloc(size_t size) {
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return memory;
}

static uint32_t read_big_endian_u32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void release_icon(void *user_data) {
    pxa_esp_package_icon_resource_t *resource =
        (pxa_esp_package_icon_resource_t *)user_data;
    if (resource == NULL) return;
    lv_image_cache_drop(&resource->descriptor);
    heap_caps_free(resource);
}

pxa_host_icon_t pxa_esp_package_icon_default(void) {
    pxa_host_icon_t result;
    memset(&result, 0, sizeof(result));
    return result;
}

pxa_host_icon_t pxa_esp_package_icon_load(const char *root,
                                          const char *relative_path) {
    static const uint8_t png_signature[] = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
    };
    pxa_host_icon_t result;
    pxa_esp_package_icon_resource_t *resource = NULL;
    FILE *file = NULL;
    long file_size;
    size_t size;
    uint32_t width;
    uint32_t height;
    lv_image_header_t decoded_header;
    char path[PXA_ESP_PACKAGE_ASSET_PATH_BYTES];
    int path_size;

    result = pxa_esp_package_icon_default();
    if (root == NULL || relative_path == NULL || root[0] == '\0' ||
        relative_path[0] == '\0') {
        return result;
    }
    path_size = snprintf(path, sizeof(path), "%s/%s", root, relative_path);
    if (path_size < 0 || (size_t)path_size >= sizeof(path)) return result;

    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) goto done;
    file_size = ftell(file);
    if (file_size <= 0 || (size_t)file_size > PXA_ESP_PACKAGE_MAX_ICON_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        goto done;
    }
    size = (size_t)file_size;
    resource = icon_alloc(sizeof(*resource) + size);
    if (resource == NULL || fread(resource->bytes, 1, size, file) != size) {
        goto done;
    }
    if (size < 24 ||
        memcmp(resource->bytes, png_signature, sizeof(png_signature)) != 0 ||
        memcmp(resource->bytes + 12, "IHDR", 4) != 0) {
        goto done;
    }
    width = read_big_endian_u32(resource->bytes + 16);
    height = read_big_endian_u32(resource->bytes + 20);
    if (width == 0 || height == 0 ||
        width > PXA_ESP_PACKAGE_MAX_ICON_DIMENSION ||
        height > PXA_ESP_PACKAGE_MAX_ICON_DIMENSION) {
        goto done;
    }

    memset(&resource->descriptor, 0, sizeof(resource->descriptor));
    resource->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    resource->descriptor.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    resource->descriptor.header.w = width;
    resource->descriptor.header.h = height;
    resource->descriptor.data_size = size;
    resource->descriptor.data = resource->bytes;
    /* RAW_ALPHA requires a registered decoder for the embedded PNG data. Do
     * not hand an undecodable descriptor to the software renderer: its raw
     * color format has no pixel size and cannot be transformed safely. */
    if (lv_image_decoder_get_info(&resource->descriptor, &decoded_header) !=
        LV_RESULT_OK) {
        goto done;
    }
    result.image_dsc = &resource->descriptor;
    result.release = release_icon;
    result.release_context = resource;
    resource = NULL;

done:
    if (file != NULL) fclose(file);
    heap_caps_free(resource);
    return result;
}

#endif /* ESP_PLATFORM */
