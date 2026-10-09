#pragma once

#include "esp_mosaico_touch_events.h"

// ESP's monotonic clock starts at boot; LVGL's tick starts at adapter init.
// Convert sample age, never the absolute boot timestamp, into LVGL ticks.
// Repeated reads of an unchanged held contact are activity at the current tick.
inline uint32_t MosaicoTouchLvglTimestamp(int64_t sample_us, int64_t read_us,
                                         uint32_t lv_now, bool queued) {
    if (!queued || sample_us <= 0 || sample_us > read_us) return lv_now;
    const uint64_t age_ms = static_cast<uint64_t>(read_us - sample_us) / 1000;
    if (age_ms > UINT32_MAX) return lv_now;
    return lv_now - static_cast<uint32_t>(age_ms);
}

// Preserve gesture endpoints when rendering delays an LVGL input read. Only
// consecutive motion samples coalesce; DOWN's original hit-test position and
// UP always survive. Producer and consumer are serialized by touch_lock_.
class EspMosaicoTouchQueue {
public:
    struct Sample {
        EspMosaicoTouchEvent event;
        int64_t timestamp_us;
        bool motion;
    };

    void Update(const EspMosaicoTouchEvent& event, int64_t timestamp_us) {
        if (producer_.pressed && event.pressed &&
            producer_.point.id != event.point.id) {
            auto release = producer_;
            release.pressed = false;
            Update(release, timestamp_us);
        }
        if (event.pressed == producer_.pressed &&
            (!event.pressed || (event.point.id == producer_.point.id &&
             event.point.x == producer_.point.x && event.point.y == producer_.point.y)))
            return;
        const bool motion = event.pressed && producer_.pressed &&
                            event.point.id == producer_.point.id;
        producer_ = event;
        if (motion && count_ && At(count_ - 1).motion) {
            At(count_ - 1) = {event, timestamp_us, true};
            return;
        }
        const bool overflow = count_ == kCapacity;
        if (overflow) {
            // Cancel a stale consumer gesture before accepting the latest
            // state, even if many taps accumulated while the UI was blocked.
            head_ = count_ = 0;
            auto release = consumed_.event;
            release.pressed = false;
            Append({release, timestamp_us, false});
        }
        Append({event, timestamp_us, motion && !overflow});
    }

    Sample Read() {
        if (count_) {
            consumed_ = At(0);
            head_ = (head_ + 1) % kCapacity;
            --count_;
        }
        return consumed_;
    }

    bool pending() const { return count_ != 0; }

private:
    static constexpr size_t kCapacity = 16;
    Sample& At(size_t offset) { return samples_[(head_ + offset) % kCapacity]; }
    void Append(const Sample& sample) { At(count_++) = sample; }
    Sample samples_[kCapacity] = {};
    EspMosaicoTouchEvent producer_ = {};
    Sample consumed_ = {};
    size_t head_ = 0;
    size_t count_ = 0;
};
