// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_pixels.h"
#include <pxa_render_accel.h>
#include <cstring>
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <src/draw/sw/blend/lv_draw_sw_blend_private.h>
#include <driver/ppa.h>
#include <freertos/semphr.h>
#endif

#if defined(ESP_PLATFORM) && PXA_ESPP4_PIXEL_ACCEL && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
extern "C" void espp4_a_fill_blocks(uint16_t*, uint32_t, const uint16_t*);
extern "C" void espp4_a_expand_blocks(void*, const void*, uint32_t, unsigned);
#define ESPP4_PIXEL_SIMD 1
#else
#define ESPP4_PIXEL_SIMD 0
#endif

bool espp4_a_pixels_simd_available() {
#if ESPP4_PIXEL_SIMD
    return xTaskGetCoreID(nullptr) != tskNO_AFFINITY;
#else
    return false;
#endif
}
void espp4_a_fill_rgb565(uint16_t* output, uint32_t count, uint16_t color) {
    if (!output || !count) return;
#if ESPP4_PIXEL_SIMD
    if (count >= 32 && espp4_a_pixels_simd_available() && !(uintptr_t(output) & 1u)) {
        while (count && (uintptr_t(output) & 15u)) { *output++ = color; --count; }
        const uint32_t blocks = count / 8;
        if (blocks) espp4_a_fill_blocks(output, blocks, &color);
        output += blocks * 8; count -= blocks * 8;
    }
#endif
    pxa_render_fill_rgb565(output, count, color);
}
void espp4_a_expand_rgb565(void* output, const void* input, uint32_t count, unsigned scale) {
    if (!output || !input || !count || (scale != 2 && scale != 4)) return;
    auto* out = static_cast<uint8_t*>(output);
    const auto* in = static_cast<const uint8_t*>(input);
#if ESPP4_PIXEL_SIMD
    // Align both pointers by consuming whole pixels. Some padded/cropped rows
    // cannot satisfy both alignments; keep their exact portable implementation.
    if (count >= 16 && espp4_a_pixels_simd_available() &&
        !(uintptr_t(in) & 1u) && !(uintptr_t(out) & 1u) &&
        !((uintptr_t(out) - uintptr_t(in) * scale) & 15u)) {
        while (count && (uintptr_t(in) & 15u)) {
            uint16_t color; std::memcpy(&color, in, 2);
            for (unsigned i = 0; i < scale; ++i) std::memcpy(out + i * 2, &color, 2);
            in += 2; out += scale * 2; --count;
        }
        const uint32_t blocks = count / 8;
        if (blocks) espp4_a_expand_blocks(out, in, blocks, scale);
        in += blocks * 16; out += blocks * 16 * scale; count -= blocks * 8;
    }
#endif
    pxa_render_expand_rgb565(out, in, count, scale, false);
}
int espp4_a_lvgl_fill(void* descriptor) {
#ifdef ESP_PLATFORM
    auto* dsc = static_cast<lv_draw_sw_blend_fill_dsc_t*>(descriptor);
    if (!dsc || !espp4_a_pixels_simd_available() || dsc->dest_w < 32 || dsc->dest_h <= 0 ||
        dsc->mask_buf || dsc->opa < LV_OPA_MAX) return LV_RESULT_INVALID;
    auto* row = static_cast<uint8_t*>(dsc->dest_buf);
    const auto color = lv_color_to_u16(dsc->color);
    if (dsc->dest_stride == dsc->dest_w * 2)
        espp4_a_fill_rgb565(reinterpret_cast<uint16_t*>(row), dsc->dest_w * dsc->dest_h, color);
    else for (int y = 0; y < dsc->dest_h; ++y, row += dsc->dest_stride)
        espp4_a_fill_rgb565(reinterpret_cast<uint16_t*>(row), dsc->dest_w, color);
    return LV_RESULT_OK;
#else
    (void)descriptor;
    return 1;
#endif
}

bool espp4_a_fill_bands(void*, uint16_t* pixels, uint32_t stride,
    uint16_t width, uint16_t height, const uint16_t* tops,
    const uint16_t* bottoms, const uint16_t* colors, uint8_t count) {
#if defined(ESP_PLATFORM) && PXA_ESPP4_PIXEL_ACCEL
    // PPA cache invalidation must never touch another object's cache line.
    if (!pixels || !tops || !bottoms || !colors || !width || !height || !count ||
        width > 4095 || height > 4095 || stride != width ||
        (uintptr_t(pixels) % CONFIG_CACHE_L2_CACHE_LINE_SIZE) ||
        (width * 2u % CONFIG_CACHE_L2_CACHE_LINE_SIZE)) return false;
    uint32_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (tops[i] >= bottoms[i] || bottoms[i] > height) return false;
        total += uint32_t(bottoms[i] - tops[i]) * width;
    }
    if (total < 65536) return false;
    struct State {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
        ppa_client_handle_t client = nullptr;
        State() {
            ppa_client_config_t config{};
            config.oper_type = PPA_OPERATION_FILL;
            config.max_pending_trans_num = 1;
            config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
            if (ppa_register_client(&config,&client) != ESP_OK) client = nullptr;
        }
    };
    static State state;
    if (!state.mutex || !state.client) return false;
    xSemaphoreTake(state.mutex,portMAX_DELAY);
    bool ok = true;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned red = colors[i] >> 11, green = (colors[i] >> 5) & 63, blue = colors[i] & 31;
        ppa_fill_oper_config_t operation{};
        operation.out.buffer = pixels; operation.out.buffer_size = size_t(width) * height * 2;
        operation.out.pic_w = width; operation.out.pic_h = height;
        operation.out.block_offset_y = tops[i]; operation.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
        operation.fill_block_w = width; operation.fill_block_h = bottoms[i] - tops[i];
        operation.fill_argb_color.a = 255;
        operation.fill_argb_color.r = (red << 3) | (red >> 2);
        operation.fill_argb_color.g = (green << 2) | (green >> 4);
        operation.fill_argb_color.b = (blue << 3) | (blue >> 2);
        operation.mode = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_fill(state.client,&operation) != ESP_OK) { ok = false; break; }
    }
    xSemaphoreGive(state.mutex);
    return ok;
#else
    (void)pixels; (void)stride; (void)width; (void)height; (void)tops;
    (void)bottoms; (void)colors; (void)count;
    return false;
#endif
}
