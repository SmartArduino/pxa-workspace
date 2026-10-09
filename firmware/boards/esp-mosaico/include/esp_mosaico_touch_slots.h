#pragma once

#include "esp_mosaico_touch_events.h"

// Each LVGL pointer keeps a controller contact until its release. A replaced
// contact spends one read in RELEASED before its slot can be used again.
class EspMosaicoTouchSlots {
public:
    void Update(const EspMosaicoTouchPoint* points, size_t count) {
        if (count && !points) return;
        count = std::min(count, EspMosaicoTouchState::kMaxPoints);
        bool used[EspMosaicoTouchState::kMaxPoints] = {};
        bool released[EspMosaicoTouchState::kMaxPoints] = {};
        for (size_t slot = 0; slot < EspMosaicoTouchState::kMaxPoints; ++slot) {
            if (!slots_[slot].pressed) continue;
            bool found = false;
            for (size_t point = 0; point < count; ++point) {
                if (!used[point] && points[point].id == slots_[slot].point.id) {
                    slots_[slot].point = points[point];
                    used[point] = found = true;
                    break;
                }
            }
            if (!found) {
                slots_[slot].pressed = false;
                released[slot] = true;
            }
        }
        for (size_t point = 0; point < count; ++point) {
            if (used[point]) continue;
            for (size_t slot = 0; slot < EspMosaicoTouchState::kMaxPoints; ++slot) {
                if (!slots_[slot].pressed && !released[slot]) {
                    slots_[slot] = {points[point], true};
                    break;
                }
            }
        }
    }

    const EspMosaicoTouchEvent& Get(size_t slot) const { return slots_[slot]; }

private:
    EspMosaicoTouchEvent slots_[EspMosaicoTouchState::kMaxPoints] = {};
};
