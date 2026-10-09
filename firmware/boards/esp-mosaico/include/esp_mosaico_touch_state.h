#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "esp_mosaico_config.h"

struct EspMosaicoTouchPoint {
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t id = 0;
};

class EspMosaicoTouchState {
public:
    static constexpr size_t kMaxPoints = 2;
    static constexpr int64_t kStaleTimeoutUs = 150000;

    void Update(bool valid, const EspMosaicoTouchPoint* points,
                size_t count, int64_t timestamp_us) {
        if (!valid || (count != 0 && points == nullptr)) return;
        if (count == 0) {
            count_ = 0;
            timestamp_us_ = timestamp_us;
            return;
        }
        EspMosaicoTouchPoint next[kMaxPoints] = {};
        size_t next_count = 0;
        for (size_t index = 0; index < std::min(count, kMaxPoints); ++index) {
            if (points[index].x >= mosaico_board::kWidth ||
                points[index].y >= mosaico_board::kHeight)
                continue;
            next[next_count++] = points[index];
        }
        if (next_count == 0) return;
        for (size_t index = 0; index < next_count; ++index)
            points_[index] = next[index];
        count_ = next_count;
        timestamp_us_ = timestamp_us;
    }

    size_t Snapshot(int64_t timestamp_us, EspMosaicoTouchPoint* points,
                     size_t capacity) const {
        if (points == nullptr || timestamp_us < timestamp_us_ ||
            timestamp_us - timestamp_us_ >= kStaleTimeoutUs)
            return 0;
        const size_t count = std::min(count_, capacity);
        for (size_t index = 0; index < count; ++index)
            points[index] = points_[index];
        return count;
    }

private:
    EspMosaicoTouchPoint points_[kMaxPoints] = {};
    size_t count_ = 0;
    int64_t timestamp_us_ = 0;
};
