// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace doit_s31 {
constexpr unsigned kTouchPointers = 10;
struct TouchContact {
    uint16_t x = 0, y = 0;
    bool pressed = false;
};
using TouchReport = std::array<TouchContact, kTouchPointers>;

// ST7102 TDDI Interface Protocol V01.01, report-page registers. Register
// 0x14 contains fixed seven-byte finger slots. Keep their indices: compacting
// valid records loses identity when an earlier finger lifts or fingers cross.
inline bool DecodeTouch(std::span<const uint8_t> bytes, uint16_t width,
                        uint16_t height, TouchReport& report) {
    if (bytes.size() % 7 || bytes.size() > kTouchPointers * 7) return false;
    TouchReport next{};
    for (size_t i = 0; i < bytes.size() / 7; ++i) {
        const auto* p = bytes.data() + i * 7;
        if (!(p[0] & 0x80)) continue;
        const uint16_t x = uint16_t((p[0] & 0x3f) << 8 | p[1]);
        const uint16_t y = uint16_t((p[2] & 0x3f) << 8 | p[3]);
        if (x >= width || y >= height) return false;
        next[i] = {x, y, true};
    }
    report = next;
    return true;
}

// Read one coherent report per UI input pass, shared by all LVGL indevs.
template<class Read>
bool ReadTouch(Read read, unsigned slots, uint16_t width, uint16_t height,
               TouchReport& report) {
    if (!slots || slots > kTouchPointers) return false;
    uint8_t status;
    if (!read(0x10, &status, 1)) return false;
    if (!(status & 8)) { report = {}; return true; }
    std::array<uint8_t, kTouchPointers * 7> bytes{};
    if (!read(0x14, bytes.data(), slots * 7)) return false;
    return DecodeTouch(std::span<const uint8_t>(bytes.data(), slots * 7), width, height, report);
}
} // namespace doit_s31
