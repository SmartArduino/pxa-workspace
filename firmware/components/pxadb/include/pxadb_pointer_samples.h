#pragma once
#include <cstddef>
#include <cstdint>

namespace pxadb {
// The PXADB producer and the LVGL reader run on different tasks. Board callers
// serialize access with their portMUX; no allocation or LVGL call occurs here.
// Keep edges until LVGL consumes them, even when DOWN and UP arrive in one tick.
template<size_t Pointers, size_t Depth = 8>
class PointerSamples {
public:
    static_assert(Pointers > 0 && Pointers <= 16 && Depth >= 3 && Depth <= 255);
    struct Sample {
        uint16_t x = 0, y = 0;
        bool pressed = false, cancel = false, move = false;
    };
    bool Push(uint8_t id, uint16_t x, uint16_t y, bool pressed) {
        if (id >= Pointers) return false;
        auto& slot = slots_[id];
        Sample sample{x, y, pressed, false, pressed && slot.accepted_pressed};
        if (sample.move && slot.count) {
            auto& tail = slot.queue[(slot.head + slot.count - 1) % Depth];
            if (tail.move) { tail = sample; return true; }
        }
        if (slot.count == Depth) return false;
        slot.queue[(slot.head + slot.count++) % Depth] = sample;
        slot.accepted_pressed = pressed;
        return true;
    }
    bool Cancel(uint8_t id) {
        if (id >= Pointers) return false;
        auto& slot = slots_[id];
        Sample release = slot.current;
        release.pressed = false; release.cancel = true; release.move = false;
        slot.head = 0; slot.count = 1; slot.queue[0] = release;
        slot.accepted_pressed = false;
        return true;
    }
    Sample Read(uint8_t id, bool& more) {
        more = false;
        if (id >= Pointers) return {};
        auto& slot = slots_[id];
        if (slot.count) {
            slot.current = slot.queue[slot.head];
            slot.head = (slot.head + 1) % Depth; --slot.count;
        } else slot.current.cancel = false;
        more = slot.count != 0;
        return slot.current;
    }
    bool active() const {
        for (const auto& slot : slots_)
            if (slot.count || slot.current.pressed || slot.accepted_pressed) return true;
        return false;
    }
    void Clear() { for (auto& slot : slots_) slot = {}; }
private:
    struct Slot {
        Sample current{}, queue[Depth]{};
        uint8_t head = 0, count = 0;
        bool accepted_pressed = false;
    } slots_[Pointers]{};
};
}
