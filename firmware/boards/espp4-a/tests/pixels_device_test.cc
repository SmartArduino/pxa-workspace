// SPDX-License-Identifier: Apache-2.0
#include "pixels_cases.h"
#include <pxa_render_accel.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {
struct State { SemaphoreHandle_t done; bool passed = false; };
bool TestFrameCopy() {
    constexpr size_t bytes = 480 * 640 * 2, extent = bytes + 256;
    auto* source = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, extent, MALLOC_CAP_SPIRAM));
    auto* output = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, extent, MALLOC_CAP_SPIRAM));
    bool passed = source && output;
    if (passed) {
        uint32_t pattern = 1;
        for (size_t i = 0; i < extent; ++i) {
            pattern = 1664525u * pattern + 1013904223u;
            source[i] = pattern >> 24;
        }
        for (size_t offset : {0u, 2u, 64u, 126u}) {
            const size_t count = bytes - offset - 3;
            std::memset(output, 0x5b, extent);
            espp4_a_copy_frame(output+offset, source+6, count);
            passed &= !std::memcmp(output+offset, source+6, count);
            for (size_t i = 0; i < offset; ++i) passed &= output[i] == 0x5b;
            for (size_t i = offset+count; i < extent; ++i) passed &= output[i] == 0x5b;
        }
        constexpr unsigned width = 852, height = 96, stride = 854 * 2;
        for (unsigned offset : {0u, 2u, 126u}) {
            std::memset(output, 0x5b, extent);
            passed &= espp4_a_copy_video_rows(output + offset, stride, source + 2, width, height);
            for (unsigned y = 0; y < height; ++y) {
                const auto *row = output + offset + size_t(y) * stride;
                passed &= !std::memcmp(row, source + 2 + size_t(y) * width * 2, width * 2);
                for (unsigned x = width * 2; x < stride; ++x) passed &= row[x] == 0x5b;
            }
            if (offset) passed &= output[offset - 1] == 0x5b;
            passed &= output[offset + height * stride] == 0x5b;
        }
        const auto start = esp_timer_get_time();
        for (unsigned i = 0; i < 16; ++i) {
            std::memcpy(output, source, bytes);
            asm volatile("" ::: "memory");
        }
        const auto middle = esp_timer_get_time();
        for (unsigned i = 0; i < 16; ++i) espp4_a_copy_frame(output, source, bytes);
        ESP_LOGI("espp4_pixels", "frame copy bytes=%u cpu_us=%lld dma_path_us=%lld guards=%s",
                 unsigned(bytes), (middle-start)/16, (esp_timer_get_time()-middle)/16,
                 passed ? "PASS" : "FAIL");
    }
    heap_caps_free(output);
    heap_caps_free(source);
    return passed;
}
void Run(void* context) {
    auto* state = static_cast<State*>(context);
    state->passed = espp4_a_pixels_simd_available() && TestEspp4Pixels() && TestFrameCopy();
    auto* data = static_cast<uint16_t*>(heap_caps_aligned_alloc(128, 153728,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* input = static_cast<uint16_t*>(heap_caps_aligned_alloc(16, 28800,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data || !input) state->passed = false;
    if (data && input) {
        for (unsigned i = 0; i < 14400; ++i) input[i] = uint16_t(i * 539u);
        int64_t fill_us[2]{}, expand_us[2]{};
        for (unsigned round = 0; round < 24; ++round) for (unsigned order = 0; order < 2; ++order) {
            const unsigned simd = (round + order) & 1u;
            int64_t start = esp_timer_get_time();
            if (simd) espp4_a_fill_rgb565(data,57600,0x64a9);
            else pxa_render_fill_rgb565(data,57600,0x64a9);
            fill_us[simd] += esp_timer_get_time() - start;
            start = esp_timer_get_time();
            if (simd) espp4_a_expand_rgb565(data,input,14400,4);
            else pxa_render_expand_rgb565(data,input,14400,4,false);
            expand_us[simd] += esp_timer_get_time() - start;
        }
        ESP_LOGI("espp4_pixels", "bench core=%d fill_cpu=%lld fill_pie=%lld expand4_cpu=%lld expand4_pie=%lld us",
            xPortGetCoreID(), fill_us[0]/24,fill_us[1]/24,expand_us[0]/24,expand_us[1]/24);
        std::fill_n(data,76864,0x6b6b);
        const uint16_t tops[] = {0,17,89}, bottoms[] = {17,89,240}, colors[] = {0x07e0,0xf81f,0x001f};
        bool ppa = espp4_a_fill_bands(nullptr,data,320,320,240,tops,bottoms,colors,3);
        for (unsigned i = 0; i < 76800; ++i)
            ppa = (data[i] == colors[i / 320 < 17 ? 0 : i / 320 < 89 ? 1 : 2]) && ppa;
        for (unsigned i = 76800; i < 76864; ++i) ppa = (data[i] == 0x6b6b) && ppa;
        ESP_LOGI("espp4_pixels", "PPA bands core=%d %s",xPortGetCoreID(),ppa ? "PASS" : "FAIL");
        state->passed = state->passed && ppa;
    }
    heap_caps_free(input); heap_caps_free(data);
    ESP_LOGI("espp4_pixels", "differential core=%d %s",xPortGetCoreID(),state->passed ? "PASS" : "FAIL");
    xSemaphoreGive(state->done); vTaskDelete(nullptr);
}
}
bool espp4_a_pixels_device_test() {
    StaticSemaphore_t storage{};
    State state{xSemaphoreCreateBinaryStatic(&storage)};
    bool passed = true;
    for (int core = 0; core < 2; ++core) {
        state.passed = false;
        if (xTaskCreatePinnedToCore(Run,"p4_pixel_test",8192,&state,4,nullptr,core) != pdPASS)
            return false;
        xSemaphoreTake(state.done,portMAX_DELAY);
        passed = passed && state.passed;
    }
    return passed;
}
