#pragma once
#include <cstddef>
#include <cstdint>

// The gesture that wakes a dark screen belongs to the lock screen wake path,
// not to a hidden app. Suppress every sample through its matching release.
class MosaicoWakeTouch {
public:
    bool Consume(size_t slot, bool pressed, bool asleep) {
        const uint8_t bit = static_cast<uint8_t>(1u << slot);
        if (pressed && (asleep || blocked_ != 0)) blocked_ |= bit;
        const bool consumed = (blocked_ & bit) != 0;
        if (!pressed) blocked_ &= static_cast<uint8_t>(~bit);
        return consumed;
    }
    bool active() const { return blocked_ != 0; }
private:
    uint8_t blocked_ = 0;
};
