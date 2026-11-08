// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <pxa_board_common/display_completion.h>

namespace doit_s31 {
// Caller serializes task/ISR access. Use two VSYNC boundaries after the RGB driver confirms
// its buffer handoff, so capture timestamps follow a complete scan.
class ScanoutClock {
public:
    struct Stats {
        uint32_t scans = 0, delayed = 0, max_interval_us = 0;
    };
    explicit ScanoutClock(uint32_t epoch = 0) : epoch_(epoch) {}
    // This entire operation must stay in the cache-safe VSYNC callback.
    __attribute__((always_inline)) void Complete(uint32_t timestamp_low) {
        if (have_previous_) {
            const uint32_t interval = timestamp_low - previous_us_;
            if (interval > stats_.max_interval_us) stats_.max_interval_us = interval;
            if (interval > 20000) ++stats_.delayed;
        }
        previous_us_ = timestamp_low; have_previous_ = true; ++stats_.scans;
        timestamps_[++epoch_ % timestamps_.size()] = timestamp_low;
    }
    Stats TakeStats() { const auto result = stats_; stats_ = {}; return result; }
    uint32_t Fence() const { return epoch_ + 2; }
    bool Timestamp(uint32_t fence, uint64_t observed_us, uint64_t& completed_us) const {
        if (int32_t(epoch_ - fence) < 0) return false;
        // A long task stall can overwrite the ring. Use the latest actual VSYNC
        // in that case, conservatively reporting a later presentation time.
        const uint32_t event = epoch_ - fence < timestamps_.size() ? fence : epoch_;
        completed_us = pxa_board_common::DisplayCompletionTimestamp(
            observed_us, timestamps_[event % timestamps_.size()]);
        return true;
    }
private:
    uint32_t epoch_;
    uint32_t previous_us_ = 0;
    bool have_previous_ = false;
    Stats stats_;
    std::array<uint32_t, 64> timestamps_{};
};
} // namespace doit_s31
