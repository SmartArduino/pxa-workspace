#pragma once

#include <cstdint>

namespace pxa_board_common {

// ISR callbacks publish a lock-free 32-bit microsecond timestamp. Reconstruct
// it at collection time, including a low-word wrap. The completion must be
// less than 2^32 microseconds old; board waits are bounded far below that.
constexpr uint64_t DisplayCompletionTimestamp(uint64_t observed_us,
                                              uint32_t completed_low) {
    return observed_us - static_cast<uint32_t>(
        static_cast<uint32_t>(observed_us) - completed_low);
}

}  // namespace pxa_board_common
