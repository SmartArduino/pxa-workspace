// SPDX-License-Identifier: Apache-2.0
// Opt-in on-device differential check; never linked into normal firmware.
#include "espp4_a_rotation.h"
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <pxa/display_rotation.h>

namespace espp4_a {
bool VerifyHardwareRotation(RotationEngine& engine, uint16_t* source, uint16_t* output,
                            uint32_t width, uint32_t height) {
    const size_t count = size_t(width) * height, bytes = count * 2;
    auto* saved = static_cast<uint16_t*>(heap_caps_aligned_alloc(
        CONFIG_CACHE_L2_CACHE_LINE_SIZE, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* expected = static_cast<uint16_t*>(heap_caps_aligned_alloc(
        CONFIG_CACHE_L2_CACHE_LINE_SIZE, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!saved || !expected) { heap_caps_free(saved); heap_caps_free(expected); return false; }
    std::memcpy(saved, source, bytes);
    bool passed = true;
    for (uint8_t rotation = 1; rotation < 4; ++rotation) {
        // Exercise full native dimensions, every RGB565 bit and partial DMA tiles.
        for (size_t i = 0; i < count; ++i)
            source[i] = uint16_t((i * 1009u) ^ (i >> 5) ^ (rotation * 0x1234u));
        int64_t at = esp_timer_get_time();
        pxa_display_rgb565_to_native(source, expected, width, height, rotation);
        const int64_t cpu_us = esp_timer_get_time() - at;
        at = esp_timer_get_time();
        const bool hardware = engine.Rotate(source, output, width, height, rotation);
        const int64_t hardware_us = esp_timer_get_time() - at;
        const bool identical = std::memcmp(expected, output, bytes) == 0;
        ESP_LOGI("espp4_rotation", "Selftest: rotation=%u cpu_us=%lld ppa_us=%lld hardware=%u identical=%u",
            unsigned(rotation), static_cast<long long>(cpu_us), static_cast<long long>(hardware_us),
            unsigned(hardware), unsigned(identical));
        passed = passed && hardware && identical;
    }
    std::memcpy(source, saved, bytes);
    heap_caps_free(expected); heap_caps_free(saved);
    (void)engine.TakeStats();
    if (!passed) engine.Disable();
    ESP_LOGI("espp4_rotation", "Hardware rotation selftest %s", passed ? "PASS" : "FAIL");
    return passed;
}
} // namespace espp4_a
