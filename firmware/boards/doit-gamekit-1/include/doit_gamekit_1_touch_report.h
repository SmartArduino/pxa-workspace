#pragma once
#include "doit_gamekit_1_config.h"
#include "doit_gamekit_1_rotation.h"
#include <array>

namespace doit_gamekit_1 {
struct TouchPoint {
    uint8_t id = 0xff;
    uint16_t x = 0, y = 0;
    bool pressed = false;
};
struct TouchReport {
    std::array<TouchPoint, 2> points{};
    size_t count = 0;
};

// FT6336U: TD_STATUS followed by two six-byte records, starting at 0x02.
inline bool DecodeTouchReport(const uint8_t* bytes, size_t size,
                              LandscapeRotation rotation, TouchReport& output) {
    output = {};
    if (!bytes || size < 13 || (bytes[0] & 0x0f) > 2) return false;
    for (size_t i = 0; i < (bytes[0] & 0x0fu); ++i) {
        const auto* point = bytes + 1 + i * 6;
        const uint8_t event = point[0] >> 6;
        if (event == 1) continue; // explicit UP; absence also releases a contact
        if (event == 3) return false;
        const int x = ((point[0] & 0x0f) << 8) | point[1];
        const int y = ((point[2] & 0x0f) << 8) | point[3];
        const uint8_t id = point[2] >> 4;
        if (x >= kTouchNativeWidth || y >= kTouchNativeHeight) return false;
        for (size_t previous = 0; previous < output.count; ++previous)
            if (output.points[previous].id == id) return false;
        auto& decoded = output.points[output.count++];
        decoded.id = id;
        decoded.pressed = true;
        // game_-console: SWAP_XY=y, MIRROR_X=n, MIRROR_Y=y. Sensor dimensions
        // stay 240x320 even though the LCD RAM window is already 320x240.
        decoded.x = rotation == LandscapeRotation::Deg270 ? y : kWidth - 1 - y;
        decoded.y = rotation == LandscapeRotation::Deg270 ? kHeight - 1 - x : x;
    }
    return true;
}

// Record order may change as fingers lift. Retain each tracking ID's LVGL slot.
class TouchSlots {
public:
    std::array<TouchPoint, 2> Update(const TouchReport& report) {
        std::array<TouchPoint, 2> next{};
        std::array<bool, 2> used{};
        for (size_t slot = 0; slot < slots_.size(); ++slot)
            for (size_t i = 0; i < report.count; ++i)
                if (slots_[slot].pressed && slots_[slot].id == report.points[i].id) {
                    next[slot] = report.points[i];
                    used[i] = true;
                    break;
                }
        for (size_t i = 0; i < report.count; ++i) {
            if (used[i]) continue;
            for (auto& slot : next)
                if (!slot.pressed) { slot = report.points[i]; break; }
        }
        slots_ = next;
        return slots_;
    }
private:
    std::array<TouchPoint, 2> slots_{};
};
}
