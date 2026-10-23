#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void __real_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t *);
void __wrap_lv_draw_sw_blend_image_to_rgb565(lv_draw_sw_blend_image_dsc_t *);

static void require(bool condition)
{
    if (!condition) abort();
}

static void pixel_test(void *parent)
{
    enum { BYTES = 4096, GUARD = 64 };
    uint8_t *original = heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *expected = heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *actual = heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *source = heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *mask = heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    require(original && expected && actual && source && mask);
    for (unsigned i = 0; i < BYTES; ++i) {
        original[i] = i * 47u + (i >> 4);
        mask[i] = i * 29u;
    }
    const unsigned widths[] = {1, 7, 8, 9, 16, 31, 32, 33};
    const unsigned offsets[] = {0, 1, 2, 3, 7, 15};
    const unsigned opacities[] = {0, 1, 127, 129, 252, 253, 255};
    const unsigned alphas[] = {0, 1, 3, 124, 128, 132, 251, 252, 255};
    unsigned cases = 0;
    for (unsigned w = 0; w < sizeof(widths)/sizeof(widths[0]); ++w)
    for (unsigned off = 0; off < sizeof(offsets)/sizeof(offsets[0]); ++off)
    for (unsigned opa = 0; opa < sizeof(opacities)/sizeof(opacities[0]); ++opa)
    for (unsigned alpha = 0; alpha <= sizeof(alphas)/sizeof(alphas[0]); ++alpha) {
        for (unsigned i = 0; i < BYTES; ++i) source[i] = i * 37u + (i >> 5);
        unsigned width = widths[w], offset = offsets[off];
        unsigned src_stride = width * 4 + offset % 7;
        for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < width; ++x)
            source[GUARD + offset + y * src_stride + x * 4 + 3] =
                alpha < sizeof(alphas)/sizeof(alphas[0]) ? alphas[alpha] : (x * 53u + y * 71u);
        memcpy(expected, original, BYTES);
        memcpy(actual, original, BYTES);
        unsigned dst_offset = GUARD + ((offset * 3) & 7) * 2;
        lv_draw_sw_blend_image_dsc_t dsc = {
            .dest_buf = expected + dst_offset, .dest_w = width, .dest_h = 5,
            .dest_stride = width * 2 + (offset % 5) * 2,
            .src_buf = source + GUARD + offset, .src_stride = src_stride,
            .src_color_format = LV_COLOR_FORMAT_ARGB8888,
            .blend_mode = LV_BLEND_MODE_NORMAL, .opa = opacities[opa],
        };
        __real_lv_draw_sw_blend_image_to_rgb565(&dsc);
        dsc.dest_buf = actual + dst_offset;
        __wrap_lv_draw_sw_blend_image_to_rgb565(&dsc);
        if (memcmp(expected, actual, BYTES)) {
            ESP_LOGE("korvo_lvgl", "PIXEL_DIFF core=%d width=%u offset=%u opa=%u alpha_case=%u",
                     xPortGetCoreID(), width, offset, opacities[opa], alpha);
            abort();
        }
        ++cases;
    }
    /* Unsupported masks, premultiplied images, and blend modes must continue
     * through native LVGL rather than treating their pixels as straight ARGB. */
    for (unsigned fallback = 0; fallback < 3; ++fallback) {
        memcpy(expected, original, BYTES); memcpy(actual, original, BYTES);
        lv_draw_sw_blend_image_dsc_t dsc = {
            .dest_buf = expected + GUARD, .dest_w = 16, .dest_h = 5, .dest_stride = 40,
            .src_buf = source + GUARD, .src_stride = 64,
            .src_color_format = fallback == 1 ? LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED : LV_COLOR_FORMAT_ARGB8888,
            .blend_mode = fallback == 2 ? LV_BLEND_MODE_ADDITIVE : LV_BLEND_MODE_NORMAL,
            .opa = 127, .mask_buf = fallback == 0 ? mask : NULL, .mask_stride = 16,
        };
        __real_lv_draw_sw_blend_image_to_rgb565(&dsc);
        dsc.dest_buf = actual + GUARD;
        __wrap_lv_draw_sw_blend_image_to_rgb565(&dsc);
        require(!memcmp(expected, actual, BYTES));
        ++cases;
    }
    ESP_LOGI("korvo_lvgl", "PIXEL_VERIFY core=%d cases=%u compared_bytes=%u guards=1 mismatches=0",
             xPortGetCoreID(), cases, cases * BYTES);
    free(mask); free(source); free(actual); free(expected); free(original);
    xTaskNotifyGive((TaskHandle_t)parent);
    vTaskDelete(NULL);
}

void korvo_lvgl_accel_self_test(void)
{
    for (int core = 0; core < 2; ++core) {
        require(xTaskCreatePinnedToCore(pixel_test, "lvgl_pixels", 4096,
                xTaskGetCurrentTaskHandle(), 1, NULL, core) == pdPASS);
        require(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000)) > 0);
        vTaskDelay(1);
    }
}
