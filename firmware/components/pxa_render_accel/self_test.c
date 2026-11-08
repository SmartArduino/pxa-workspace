/* SPDX-License-Identifier: MIT */
#include "pxa_render_accel.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_PXA_RENDER_ACCEL_SELF_TEST) && CONFIG_PXA_RENDER_ACCEL_SELF_TEST
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

bool pxa_render_cordic_test(void);
void pxa_render_lvgl_test_fallback(bool enabled);

static bool test_failed;

/* Reference portable paired-store loops on the SAME core and buffers.
 * Alternate order to reduce warm-cache and background-load bias. */
static __attribute__((noinline)) void scalar_expand4(uint16_t *dst, const uint16_t *src, unsigned count) {
    uint32_t *out = __builtin_assume_aligned(dst, 4);
    const uint32_t *in = __builtin_assume_aligned(src, 4);
    for (unsigned i = 0; i < count; i += 2) {
        uint32_t pair; memcpy(&pair, in++, 4);
        pair = ((pair & 0x00ff00ffu) << 8) | ((pair >> 8) & 0x00ff00ffu);
        uint32_t low = (pair & 0xffffu) * 0x00010001u;
        uint32_t high = (pair >> 16) * 0x00010001u;
        memcpy(out++, &low, 4); memcpy(out++, &low, 4);
        memcpy(out++, &high, 4); memcpy(out++, &high, 4);
    }
}
static __attribute__((noinline)) void scalar_fill(uint16_t *dst, unsigned count, uint16_t color) {
    uint32_t pair = color | ((uint32_t)color << 16);
    uint32_t *out = __builtin_assume_aligned(dst, 4);
    for (unsigned i = 0; i < count; i += 2) memcpy(out++, &pair, 4);
}
static void benchmark(uint16_t *dst, const uint16_t *src) {
    int64_t expand_us[2] = {0}, fill_us[2] = {0}, small_fill_us[2] = {0};
    for (unsigned repeat = 0; repeat < 60; ++repeat) {
        for (unsigned order = 0; order < 2; ++order) {
            unsigned accelerated = (repeat + order) & 1u;
            int64_t start = esp_timer_get_time();
            if (accelerated) pxa_render_expand_rgb565(dst, src, 14400, 4, true);
            else scalar_expand4(dst, src, 14400);
            expand_us[accelerated] += esp_timer_get_time() - start;
            start = esp_timer_get_time();
            if (accelerated) pxa_render_fill_rgb565(dst, 57600, 0xa37c);
            else scalar_fill(dst, 57600, 0xa37c);
            fill_us[accelerated] += esp_timer_get_time() - start;
            /* Warm a cache-sized region before timing smaller solid spans. */
            scalar_fill(dst, 4096, 0x172f);
            start = esp_timer_get_time();
            if (accelerated) pxa_render_fill_rgb565(dst, 4096, 0xa37c);
            else scalar_fill(dst, 4096, 0xa37c);
            small_fill_us[accelerated] += esp_timer_get_time() - start;
        }
    }
    ESP_LOGI("render_accel", "same-core benchmark core=%d expand4 CPU=%lld accel=%lld us; fill CPU=%lld accel=%lld us",
             xPortGetCoreID(), expand_us[0]/60, expand_us[1]/60, fill_us[0]/60, fill_us[1]/60);
    ESP_LOGI("render_accel", "cached fill 4096 pixels core=%d CPU=%lld accel=%lld us",
             xPortGetCoreID(), small_fill_us[0]/60, small_fill_us[1]/60);
}

static bool benchmark_ppa_fill(uint16_t *dst) {
    const uint16_t widths[] = {128, 320, 480}, heights[] = {128, 240, 480};
    for (unsigned size = 0; size < 3; ++size) {
        uint16_t top = 0, bottom = heights[size], color = 0xad37;
        uint32_t count = (uint32_t)widths[size] * bottom;
        if (count < 65536) continue;
        int64_t elapsed[2] = {0};
        for (unsigned repeat = 0; repeat < 20; ++repeat) {
            for (unsigned order = 0; order < 2; ++order) {
                unsigned ppa = (repeat + order) & 1u;
                int64_t start = esp_timer_get_time();
                if (ppa) {
                    if (!pxa_render_fill_bands(NULL, dst, widths[size], widths[size], bottom,
                                               &top, &bottom, &color, 1)) return false;
                } else pxa_render_fill_rgb565(dst, count, color);
                elapsed[ppa] += esp_timer_get_time() - start;
            }
        }
        ESP_LOGI("render_accel", "fill %ux%u core=%d CPU/SIMD=%lld PPA=%lld us",
                 widths[size], bottom, xPortGetCoreID(), elapsed[0]/20, elapsed[1]/20);
    }
    return true;
}

static void benchmark_mix(uint16_t *dst) {
    /* Time LVGL's actual cached/paired reference with the custom hook disabled
     * only for this task, then the exact same descriptor through the hook. */
    lv_draw_sw_blend_fill_dsc_t dsc = {
        .dest_buf = dst, .dest_w = 256, .dest_h = 16, .dest_stride = 512,
        .color = lv_color_hex(0xa03c79), .opa = 153,
    };
    for (unsigned pattern = 0; pattern < 2; ++pattern) {
        int64_t elapsed[2] = {0};
        for (unsigned repeat = 0; repeat < 100; ++repeat)
            for (unsigned order = 0; order < 2; ++order) {
                unsigned accelerated = (repeat + order) & 1;
                for (unsigned i = 0; i < 4096; ++i) dst[i] = pattern ? (uint16_t)(i * 32771u) : 0x9a37;
                pxa_render_lvgl_test_fallback(!accelerated);
                int64_t start = esp_timer_get_time();
                lv_draw_sw_blend_color_to_rgb565(&dsc);
                elapsed[accelerated] += esp_timer_get_time() - start;
            }
        ESP_LOGI("render_accel", "LVGL mix 4096 %s core=%d scalar=%lld accel=%lld us",
                 pattern ? "varying" : "flat", xPortGetCoreID(), elapsed[0]/100, elapsed[1]/100);
    }
    pxa_render_lvgl_test_fallback(false);
}

static bool test_lvgl_mix(uint16_t *reference, uint16_t *accelerated) {
    const int widths[] = {1, 31, 32, 63, 64, 65, 127};
    uint8_t mask[512];
    for (unsigned i = 0; i < sizeof(mask); ++i) mask[i] = (uint8_t)(i * 61);
    for (unsigned w = 0; w < sizeof(widths)/sizeof(widths[0]); ++w)
        for (unsigned opacity = 0; opacity < 256; ++opacity)
            for (unsigned masked = 0; masked < 2; ++masked) {
                for (unsigned i = 0; i < 512; ++i) reference[i] = accelerated[i] = (uint16_t)(i * 32771u + 137u * opacity);
                lv_draw_sw_blend_fill_dsc_t dsc = {
                    .dest_buf = reference + 3, .dest_w = widths[w], .dest_h = 3, .dest_stride = 286,
                    .color = lv_color_hex(0x9e3cad), .opa = opacity,
                    .mask_buf = masked ? mask : NULL, .mask_stride = 143,
                };
                pxa_render_lvgl_test_fallback(true);
                lv_draw_sw_blend_color_to_rgb565(&dsc);
                pxa_render_lvgl_test_fallback(false);
                dsc.dest_buf = accelerated + 3;
                lv_draw_sw_blend_color_to_rgb565(&dsc);
                if (memcmp(reference, accelerated, 1024)) return false;
            }
    return true;
}
#define CHECK(condition) do { if (!(condition)) { \
    ESP_LOGE("render_accel", "FAIL core=%d line=%d: %s", xPortGetCoreID(), __LINE__, #condition); \
    test_failed = true; goto cleanup; } } while (0)
static void run(void *arg) {
    uint16_t *src = heap_caps_aligned_alloc(64, 32768, MALLOC_CAP_SPIRAM);
    uint16_t *dst = heap_caps_aligned_alloc(64, 524288, MALLOC_CAP_SPIRAM);
    CHECK(src && dst);
    ESP_LOGI("render_accel", "pixel differential begin core=%d", xPortGetCoreID());
    const uint16_t mix_colors[] = {0, 0xffff, 0xf800, 0x07e0, 0x001f, 0x172f, 0xa37c};
    for (unsigned opacity = 0; opacity < 256; ++opacity)
        for (unsigned c = 0; c < sizeof(mix_colors)/sizeof(mix_colors[0]); ++c)
            for (unsigned off = 0; off < 8; ++off) {
                unsigned count = 64 + ((opacity + off) % 66);
                for (unsigned i = 0; i < 160; ++i) dst[i] = (uint16_t)(i * 32771u + opacity * 137u);
                pxa_render_mix_rgb565(dst + off, count, mix_colors[c], opacity);
                for (unsigned i = 0; i < 160; ++i) {
                    uint16_t expected = (uint16_t)(i * 32771u + opacity * 137u);
                    if (i >= off && i < off + count) expected = lv_color_16_16_mix(mix_colors[c], expected, opacity);
                    if (dst[i] != expected) ESP_LOGE("render_accel", "mix off=%u n=%u color=%04x opa=%u i=%u got=%04x expected=%04x",
                        off, count, mix_colors[c], opacity, i, dst[i], expected);
                    CHECK(dst[i] == expected);
                }
            }
    CHECK(test_lvgl_mix(src, dst));
    for (unsigned i = 0; i < 16384; ++i) src[i] = (uint16_t)(i * 32771u + 12345u);
    for (unsigned n = 0; n < 130; ++n) {
        memcpy(dst, src, n * 2);
        uint32_t first = 777, last = 888;
        CHECK(!pxa_render_diff_rgb565(src, dst, n, &first, &last));
        CHECK(first == 777 && last == 888);
        if (n) {
            dst[n / 3] ^= 0x20;
            dst[n - 1] ^= 0x400;
            CHECK(pxa_render_diff_rgb565(src, dst, n, &first, &last));
            CHECK(first == n / 3 && last == n - 1);
        }
    }
    for (unsigned off = 0; off < 16; ++off) {
        for (unsigned n = 0; n < 130; ++n) {
            memset(dst, 0x5a, 2048);
            pxa_render_fill_rgb565(dst + off, n, 0xa37c);
            for (unsigned i = 0; i < 256; ++i)
                CHECK(dst[i] == (i >= off && i < off + n ? 0xa37c : 0x5a5a));
            pxa_render_swap_rgb565(dst + off, src + off, n);
            for (unsigned i = 0; i < n; ++i) CHECK(dst[off+i] == __builtin_bswap16(src[off+i]));
            pxa_render_swap_rgb565(dst + off, dst + off, n);
            for (unsigned i = 0; i < n; ++i) CHECK(dst[off+i] == src[off+i]);
            for (unsigned scale = 2; scale <= 4; scale += 2) {
                for (unsigned swap = 0; swap < 2; ++swap) {
                    memset(dst, 0x5a, 2048);
                    pxa_render_expand_rgb565(dst + off, src + off, n, scale, swap);
                    for (unsigned i = 0; i < n * scale; ++i) {
                        uint16_t v = src[off+i/scale];
                        if (dst[off+i] != (swap ? __builtin_bswap16(v) : v))
                            ESP_LOGE("render_accel", "expand offset=%u count=%u scale=%u swap=%u index=%u got=%04x expected=%04x", off, n, scale, swap, i, dst[off+i], swap ? __builtin_bswap16(v) : v);
                        CHECK(dst[off+i] == (swap ? __builtin_bswap16(v) : v));
                    }
                    CHECK(dst[off+n*scale] == 0x5a5a);
                }
            }
        }
    }
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) palette[i] = (uint16_t)(i * 251u);
    CHECK(pxa_render_index8_rgb565(dst, (uint8_t *)src, palette, 128, 128));
    for (unsigned i = 0; i < 16384; ++i) CHECK(dst[i] == palette[((uint8_t *)src)[i]]);
    int64_t indexed_start = esp_timer_get_time();
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        for (unsigned i = 0; i < 16384; ++i) dst[i] = palette[((uint8_t *)src)[i]];
    int64_t indexed_cpu = (esp_timer_get_time() - indexed_start) / 20;
    indexed_start = esp_timer_get_time();
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        CHECK(pxa_render_index8_rgb565(dst, (uint8_t *)src, palette, 128, 128));
    ESP_LOGI("render_accel", "INDEX8 128x128 core=%d CPU=%lld us PPA=%lld us", xPortGetCoreID(), indexed_cpu, (esp_timer_get_time()-indexed_start)/20);
    memset(dst, 0x5a, 131072);
    uint16_t tops[] = {0, 128}, bottoms[] = {128, 256}, colors[] = {0xa37c, 0x172f};
    CHECK(!pxa_render_fill_bands(NULL, dst + 1, 256, 256, 256, tops, bottoms, colors, 2));
    CHECK(!pxa_render_fill_bands(NULL, dst, 257, 256, 256, tops, bottoms, colors, 2));
    uint16_t invalid_bottoms[] = {128, 257};
    CHECK(!pxa_render_fill_bands(NULL, dst, 256, 256, 256, tops, invalid_bottoms, colors, 2));
    CHECK(!pxa_render_fill_bands(NULL, dst, 128, 128, 128, tops, bottoms, colors, 1));
    for (unsigned i = 0; i < 65536; ++i) CHECK(dst[i] == 0x5a5a);
    CHECK(pxa_render_fill_bands(NULL, dst, 256, 256, 256, tops, bottoms, colors, 2));
    for (unsigned i = 0; i < 65536; ++i) CHECK(dst[i] == colors[i / 32768]);
    benchmark(dst, src);
    benchmark_mix(dst);
    CHECK(benchmark_ppa_fill(dst));
    CHECK(pxa_render_cordic_test());
    ESP_LOGI("render_accel", "self-test PASS core=%d", xPortGetCoreID());
cleanup:
    heap_caps_free(src); heap_caps_free(dst);
    xSemaphoreGive((SemaphoreHandle_t)arg);
    vTaskDelete(NULL);
}
bool pxa_render_accel_self_test(void) {
    test_failed = false;
    StaticSemaphore_t storage;
    SemaphoreHandle_t done = xSemaphoreCreateBinaryStatic(&storage);
    for (int core = 0; core <= 1; ++core) {
        if (xTaskCreatePinnedToCore(run, "render_test", 6144, done, 3, NULL, core) != pdPASS) { test_failed = true; break; }
        xSemaphoreTake(done, portMAX_DELAY);
    }
    vSemaphoreDelete(done);
    return !test_failed;
}
#else
bool pxa_render_accel_self_test(void) { return true; }
#endif
