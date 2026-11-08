/* SPDX-License-Identifier: MIT */
#include "pxa_render_accel.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(CONFIG_PXA_RENDER_ACCEL) && CONFIG_PXA_RENDER_ACCEL
#include <pthread.h>
#include <stdatomic.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static pthread_once_t once = PTHREAD_ONCE_INIT;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static ppa_client_handle_t fill_client, blend_client;
static bool blend_attempted;
static atomic_uint fill_count, index_count;

static void initialize(void) {
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    ppa_client_config_t config = {
        .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&config, &fill_client) != ESP_OK) fill_client = NULL;
}

static color_pixel_argb8888_data_t expand(uint16_t color) {
    color_pixel_argb8888_data_t argb = {0};
    argb.a = 255;
    unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
    argb.r = (r << 3) | (r >> 2);
    argb.g = (g << 2) | (g >> 4);
    argb.b = (b << 3) | (b >> 2);
    return argb;
}

bool pxa_render_fill_bands(void *context, uint16_t *pixels, uint32_t stride,
                          uint16_t width, uint16_t height, const uint16_t *tops,
                          const uint16_t *bottoms, const uint16_t *colors,
                          uint8_t count) {
    (void)context;
    if (!pixels || !tops || !bottoms || !colors || !count || !width || !height ||
        stride != width || width > 4095 || height > 4095 ||
        ((uintptr_t)pixels & 63u) || ((width * 2u) & 63u)) return false;
    uint32_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (tops[i] >= bottoms[i] || bottoms[i] > height) return false;
        total += (uint32_t)(bottoms[i] - tops[i]) * width;
    }
    /* Setup/cache maintenance loses to cached CPU/SIMD fills on small scenes. */
    if (total < 65536) return false;
    pthread_once(&once, initialize);
    if (!fill_client) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = true;
    for (unsigned i = 0; i < count; ++i) {
        ppa_fill_oper_config_t config = {
            .out = {.buffer = pixels, .buffer_size = (size_t)width * height * 2,
                    .pic_w = width, .pic_h = height,
                    .block_offset_y = tops[i], .fill_cm = PPA_FILL_COLOR_MODE_RGB565},
            .fill_block_w = width, .fill_block_h = bottoms[i] - tops[i],
            .fill_argb_color = expand(colors[i]), .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_fill(fill_client, &config) != ESP_OK) { ok = false; break; }
    }
    if (ok) atomic_fetch_add_explicit(&fill_count, 1, memory_order_relaxed);
    xSemaphoreGive(mutex);
    return ok;
}

bool pxa_render_index8_rgb565(uint16_t *dst, const uint8_t *src,
                             const uint16_t *palette, uint16_t width,
                             uint16_t height) {
    /* Whole cache lines only: IDF invalidates the destination before DMA.
     * Guest mappings with padding or shared edge cache lines use the CPU. */
    if (!dst || !src || !palette || !width || !height || width > 4095 || height > 4095 ||
        (uint32_t)width * height < 4096 || ((uintptr_t)dst & 63u) ||
        ((width * 2u) & 63u)) return false;
    pthread_once(&once, initialize);
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!blend_attempted) {
        blend_attempted = true;
        ppa_client_config_t client = {
            .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
        };
        if (ppa_register_client(&client, &blend_client) != ESP_OK) blend_client = NULL;
    }
    if (!blend_client) { xSemaphoreGive(mutex); return false; }
    color_pixel_argb8888_data_t colors[256];
    for (unsigned i = 0; i < 256; ++i) colors[i] = expand(palette[i]);
    /* This backend owns the background CLUT for the entire blocking operation.
     * The foreground is the same INDEX8 bytes interpreted as an A8 mask with
     * alpha forced to zero; it cannot affect the opaque background resolve. */
    ppa_blend_oper_config_t config = {
        .in_bg = {.buffer = src, .pic_w = width, .pic_h = height,
                  .block_w = width, .block_h = height, .blend_cm = PPA_BLEND_COLOR_MODE_L8},
        .in_fg = {.buffer = src, .pic_w = width, .pic_h = height,
                  .block_w = width, .block_h = height, .blend_cm = PPA_BLEND_COLOR_MODE_A8},
        .out = {.buffer = dst, .buffer_size = (size_t)width * height * 2,
                .pic_w = width, .pic_h = height, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565},
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .fg_alpha_fix_val = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    bool ok = ppa_set_color_lookup_table(PPA_CLUT_BLEND_BG, colors, 256) == ESP_OK &&
              ppa_do_blend(blend_client, &config) == ESP_OK;
    if (ok) atomic_fetch_add_explicit(&index_count, 1, memory_order_relaxed);
    xSemaphoreGive(mutex);
    return ok;
}
uint32_t pxa_render_ppa_fill_count(void) { return atomic_load_explicit(&fill_count, memory_order_relaxed); }
uint32_t pxa_render_ppa_index_count(void) { return atomic_load_explicit(&index_count, memory_order_relaxed); }
#else
bool pxa_render_fill_bands(void *context, uint16_t *pixels, uint32_t stride,
                          uint16_t width, uint16_t height, const uint16_t *tops,
                          const uint16_t *bottoms, const uint16_t *colors, uint8_t count) {
    (void)context; (void)pixels; (void)stride; (void)width; (void)height;
    (void)tops; (void)bottoms; (void)colors; (void)count; return false;
}
bool pxa_render_index8_rgb565(uint16_t *dst, const uint8_t *src,
                             const uint16_t *palette, uint16_t width, uint16_t height) {
    (void)dst; (void)src; (void)palette; (void)width; (void)height; return false;
}
uint32_t pxa_render_ppa_fill_count(void) { return 0; }
uint32_t pxa_render_ppa_index_count(void) { return 0; }
#endif
