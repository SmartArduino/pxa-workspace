#pragma once

#include <algorithm>
#include <cstddef>

#include "esp_mosaico_touch_state.h"

struct EspMosaicoTouchEvent {
    EspMosaicoTouchPoint point;
    bool pressed = false;
};

class EspMosaicoTouchEvents {
public:
    static constexpr size_t kMaxEvents = EspMosaicoTouchState::kMaxPoints * 2;

    size_t Update(const EspMosaicoTouchPoint* points, size_t count,
                  EspMosaicoTouchEvent* events) {
        if (events == nullptr || (count != 0 && points == nullptr)) return 0;
        count = std::min(count, EspMosaicoTouchState::kMaxPoints);
        size_t event_count = 0;
        for (size_t previous = 0; previous < count_; ++previous) {
            bool found = false;
            for (size_t current = 0; current < count; ++current) {
                if (points[current].id == points_[previous].id) found = true;
            }
            if (!found) events[event_count++] = {points_[previous], false};
        }
        for (size_t current = 0; current < count; ++current) {
            events[event_count++] = {points[current], true};
            points_[current] = points[current];
        }
        count_ = count;
        if (count > 0) pointer_point_ = points[0];
        return event_count;
    }

    const EspMosaicoTouchPoint& pointer_point() const { return pointer_point_; }

private:
    EspMosaicoTouchPoint points_[EspMosaicoTouchState::kMaxPoints] = {};
    EspMosaicoTouchPoint pointer_point_;
    size_t count_ = 0;
};
