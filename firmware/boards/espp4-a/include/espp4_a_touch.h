// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace espp4_a {
constexpr unsigned kTouchPointers = 5;
struct TouchContact {
    uint16_t x = 0, y = 0;
    bool pressed = false;
};
using TouchReport = std::array<TouchContact, kTouchPointers>;

// IRQs wake the UI immediately; active contacts retain a bounded polling
// fallback. Idle controllers need only a health read, and NACKs back off.
class TouchSchedule {
public:
    void Request() { requested_ = true; }
    bool Due(uint32_t now, bool active, bool asserted, bool irq_observed = true) const {
        const uint32_t elapsed = now - attempted_;
        if (retry_ms_) return requested_ && elapsed >= retry_ms_;
        return elapsed >= (requested_ ? 5u : active || asserted || !irq_observed ? 16u : 500u);
    }
    void Begin(uint32_t now) { attempted_ = now; requested_ = false; }
    void Complete(bool success) {
        if (success) { retry_ms_ = 0; return; }
        requested_ = true;
        retry_ms_ = retry_ms_ ? (retry_ms_ < 80 ? retry_ms_ * 2 : 160) : 10;
    }
    bool Failed() const { return retry_ms_ != 0; }
private:
    uint32_t attempted_ = 0;
    uint16_t retry_ms_ = 0;
    bool requested_ = true;
};

// ST7123 1.0.2 register layout from Espressif's Apache-2.0 driver. Register
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
        const uint16_t y = uint16_t(p[2] << 8 | p[3]);
        if (x >= width || y >= height) return false;
        next[i] = {x, y, true};
    }
    report = next;
    return true;
}

// Read one coherent report per UI input pass, shared by all five LVGL indevs.
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
} // namespace espp4_a
