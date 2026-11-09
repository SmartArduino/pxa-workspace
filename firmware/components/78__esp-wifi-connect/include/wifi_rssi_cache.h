#pragma once

#include <cstdint>

// Called under WifiManager's mutex. Reuse the existing status poll instead of
// adding a timer/task or querying remote Wi-Fi hardware on every UI refresh.
class WifiRssiCache {
public:
    static constexpr int64_t kSampleIntervalUs = 5 * 1000 * 1000;
    static constexpr int kUnavailable = -127;

    void Reset() noexcept { sampled_ = false; value_ = kUnavailable; }

    template<class Read> int Get(int64_t now_us, Read read) {
        if (!sampled_ || now_us < sampled_at_ || now_us - sampled_at_ >= kSampleIntervalUs) {
            const int rssi = read();
            // A failed driver query returns zero. Never turn that into an
            // apparent maximum-strength signal; retain the last valid sample.
            if (rssi < 0) value_ = rssi;
            sampled_at_ = now_us;
            sampled_ = true;
        }
        return value_;
    }

private:
    int64_t sampled_at_ = 0;
    int value_ = kUnavailable;
    bool sampled_ = false;
};
