// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_pixels.h"
#include <pxa_render_accel.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "espp4_a_ppa_rows.h"
#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <src/draw/sw/blend/lv_draw_sw_blend_private.h>
#include <driver/ppa.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <esp_log.h>
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

#if defined(ESP_PLATFORM) && PXA_ESPP4_LVGL_PPA_FILL
namespace {
bool PpaRowsHardware(uint16_t* pixels, unsigned width, unsigned height, uint16_t color) {
    struct Client {
        StaticSemaphore_t storage{};
        SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
        ppa_client_handle_t handle = nullptr;
        std::atomic<bool> failed{false};
        Client() {
            ppa_client_config_t c{};
            c.oper_type = PPA_OPERATION_FILL;
            c.max_pending_trans_num = 1;
            c.data_burst_length = PPA_DATA_BURST_LENGTH_128;
            if (ppa_register_client(&c, &handle) != ESP_OK)
                handle = nullptr;
        }
    };
    static Client client;
    if (!client.mutex || !client.handle || client.failed.load(std::memory_order_relaxed))
        return false;
    ppa_fill_oper_config_t op{};
    op.out.buffer = pixels;
    op.out.buffer_size = size_t(width) * height * 2;
    op.out.pic_w = width;
    op.out.pic_h = height;
    op.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
    op.fill_block_w = width;
    op.fill_block_h = height;
    const unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
    op.fill_argb_color.a = 255;
    op.fill_argb_color.r = (r << 3) | (r >> 2);
    op.fill_argb_color.g = (g << 2) | (g >> 4);
    op.fill_argb_color.b = (b << 3) | (b >> 2);
    op.mode = PPA_TRANS_MODE_BLOCKING;
    xSemaphoreTake(client.mutex, portMAX_DELAY);
    const bool ok =
        !client.failed.load(std::memory_order_relaxed) && ppa_do_fill(client.handle, &op) == ESP_OK;
    if (!ok)
        client.failed.store(true, std::memory_order_relaxed);
    xSemaphoreGive(client.mutex);
    return ok;
}

// The fill rectangle owns the DMA window, including every cache line that
// IDF invalidates. Keep unaligned edge rows on the CPU; do not round outwards
// into another widget's pixels. A failed operation falls through to a complete
// CPU repaint of this opaque rectangle.
bool ApplyPpaRows(uint16_t* pixels, unsigned width, unsigned height, uint16_t color) {
    espp4_a::PpaRows plan;
    if (!espp4_a::PlanPpaRows(uintptr_t(pixels), width, height, size_t(width) * 2,
                              CONFIG_CACHE_L2_CACHE_LINE_SIZE, plan))
        return false;
    if (!PpaRowsHardware(pixels + size_t(plan.prefix) * width, width, plan.rows, color))
        return false;
    espp4_a_fill_rgb565(pixels, width * plan.prefix, color);
    espp4_a_fill_rgb565(pixels + size_t(plan.prefix + plan.rows) * width, width * plan.tail, color);
    return true;
}
#if PXA_ESPP4_LVGL_PPA_SELFTEST
bool PpaRowsSelfTest() {
    constexpr unsigned width = 480, max_height = 143, guard = 128;
    constexpr size_t bytes = size_t(width) * max_height * 2 + guard * 3;
    auto* actual =
        static_cast<uint16_t*>(heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* expected = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!actual || !expected) {
        heap_caps_free(actual);
        heap_caps_free(expected);
        return false;
    }
    bool ok = true;
    for (unsigned offset : {0u, 32u})
        for (unsigned height : {140u, 141u, 142u})
            for (uint16_t color : {uint16_t(0), uint16_t(0xffff), uint16_t(0x53a7)}) {
                for (size_t i = 0; i < bytes / 2; ++i)
                    actual[i] = expected[i] = uint16_t(i * 2317u + 0x352a);
                auto* target = actual + guard / 2 + offset;
                std::fill_n(expected + guard / 2 + offset, size_t(width) * height, color);
                if (!ApplyPpaRows(target, width, height, color) || std::memcmp(actual, expected, bytes)) {
                    ok = false;
                    break;
                }
            }
    heap_caps_free(actual);
    heap_caps_free(expected);
    ESP_LOGI("p4_fill", "PPA RGB565 18 alignment/color/full-guard cases %s", ok ? "PASS" : "FAIL");
    return ok;
}
#endif
bool NativePpaFill(lv_draw_sw_blend_fill_dsc_t* dsc) {
    if (!dsc || !dsc->dest_buf || dsc->mask_buf || dsc->opa < LV_OPA_MAX || dsc->dest_w <= 0 ||
        dsc->dest_h <= 0 || size_t(dsc->dest_stride) != size_t(dsc->dest_w) * 2 ||
        !esp_ptr_external_ram(dsc->dest_buf))
        return false;
    espp4_a::PpaRows eligible;
    if (!espp4_a::PlanPpaRows(uintptr_t(dsc->dest_buf), dsc->dest_w, dsc->dest_h, dsc->dest_stride,
                              CONFIG_CACHE_L2_CACHE_LINE_SIZE, eligible))
        return false;
#if PXA_ESPP4_LVGL_PPA_SELFTEST
    static const bool verified = PpaRowsSelfTest();
    if (!verified)
        return false;
    const int64_t started = esp_timer_get_time();
#endif
    if (!ApplyPpaRows(static_cast<uint16_t*>(dsc->dest_buf), dsc->dest_w, dsc->dest_h,
                      lv_color_to_u16(dsc->color)))
        return false;
#if PXA_ESPP4_LVGL_PPA_SELFTEST
    static std::atomic<unsigned> count{0};
    const unsigned calls = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((calls & (calls - 1)) == 0)
        ESP_LOGI("p4_fill", "PPA count=%u w=%ld h=%ld us=%lld", calls, (long)dsc->dest_w, (long)dsc->dest_h,
                 (long long)(esp_timer_get_time() - started));
#endif
    return true;
}
} // namespace
#endif

int espp4_a_lvgl_fill(void* descriptor) {
#ifdef ESP_PLATFORM
    auto* dsc = static_cast<lv_draw_sw_blend_fill_dsc_t*>(descriptor);
    if (!dsc || !espp4_a_pixels_simd_available() || dsc->dest_w < 32 || dsc->dest_h <= 0 ||
        dsc->mask_buf || dsc->opa < LV_OPA_MAX) return LV_RESULT_INVALID;
#if PXA_ESPP4_LVGL_PPA_FILL
    if (NativePpaFill(dsc)) return LV_RESULT_OK;
#endif
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
