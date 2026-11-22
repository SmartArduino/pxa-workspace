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
    // The reporting table is latched until the last supported slot is read.
    // Fetch its four-byte header, fixed slots and trailing checksum together.
    std::array<uint8_t, 5 + kTouchPointers * 7> bytes;
    bytes.fill(0xff);
    if (!read(0x10, bytes.data(), 5 + slots * 7)) return false;
    const uint8_t status = bytes[0];
    if (status & 0x80) return false; // Controller requests reset; do not publish stale data.
    // A proximity/gesture event (or no new IRQ) is not a finger release.
    // Only a coordinate report with all slots invalid releases the contacts.
    if (!(status & 8)) return true;
    return DecodeTouch(std::span<const uint8_t>(bytes.data() + 4, slots * 7), width, height, report);
}
} // namespace doit_s31
