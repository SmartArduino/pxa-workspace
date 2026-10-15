#pragma once

#include "esp_mosaico_config.h"
#include "esp_mosaico_touch_queue.h"
#include "esp_mosaico_touch_slots.h"

// Physical and PXADB input share the same two LVGL pointer devices and queues.
// Callers serialize this state with touch_lock_. No LVGL calls occur here.
class EspMosaicoInput {
public:
    static constexpr size_t kPointers = EspMosaicoTouchState::kMaxPoints;

    // Returns true when a real touch takes ownership back from PXADB.
    bool UpdatePhysical(const EspMosaicoTouchPoint* points, size_t count,
                        int64_t timestamp_us, bool valid_report) {
        slots_.Update(points, count);
        const bool takeover = injected_ && valid_report && count != 0;
        if (takeover) {
            ReleaseAll(timestamp_us);
            injected_ = false;
        }
        if (!injected_)
            for (size_t slot = 0; slot < kPointers; ++slot)
                queues_[slot].Update(slots_.Get(slot), timestamp_us);
        return takeover;
    }

    bool Inject(uint8_t id, uint16_t x, uint16_t y, bool pressed, int64_t timestamp_us) {
        if (id >= kPointers || x >= mosaico_board::kWidth || y >= mosaico_board::kHeight)
            return false;
        if (!injected_) {
            ReleaseAll(timestamp_us);
            injected_ = true;
        }
        queues_[id].Update({{x, y, id}, pressed}, timestamp_us);
        const uint8_t bit = 1u << id;
        pressed_ = pressed ? pressed_ | bit : pressed_ & ~bit;
        return true;
    }

    bool Cancel(uint8_t id, int64_t timestamp_us) {
        if (id >= kPointers) return false;
        if (injected_) {
            queues_[id].ResetWithRelease(timestamp_us);
            cancelled_ |= 1u << id;
            pressed_ &= ~(1u << id);
        }
        return true;
    }

    EspMosaicoTouchQueue::Sample Read(size_t slot) {
        auto sample = queues_[slot].Read();
        if (injected_ && pressed_ == 0 && !queues_[0].pending() && !queues_[1].pending())
            injected_ = false;
        return sample;
    }

    bool pending(size_t slot) const { return queues_[slot].pending(); }
    bool TakeCancelled(size_t slot) {
        const uint8_t bit = 1u << slot;
        const bool cancelled = (cancelled_ & bit) != 0;
        cancelled_ &= ~bit;
        return cancelled;
    }

    size_t Points(EspMosaicoTouchPoint (&points)[kPointers]) const {
        size_t count = 0;
        for (const auto& queue : queues_) {
            const auto event = queue.current();
            if (event.pressed) points[count++] = event.point;
        }
        return count;
    }

private:
    void ReleaseAll(int64_t timestamp_us) {
        for (auto& queue : queues_) queue.ResetWithRelease(timestamp_us);
        pressed_ = 0;
        cancelled_ = (1u << kPointers) - 1;
    }
    EspMosaicoTouchSlots slots_;
    EspMosaicoTouchQueue queues_[kPointers];
    uint8_t pressed_ = 0;
    uint8_t cancelled_ = 0;
    bool injected_ = false;
};
