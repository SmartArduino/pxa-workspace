#pragma once
#include <array>
#include <cstdint>

namespace mosaico_board {
// Published device frame with the panel in its normal orientation:
// Android axes: +X toward the right edge, +Y toward the top edge, +Z out.
// The BMI270 mounting already matches this native device frame. Keep both
// motion channels in that fixed right-handed frame when the UI rotates.
constexpr std::array<int32_t, 3> ImuDeviceFrame(int32_t x, int32_t y, int32_t z) {
    // Inputs are scaled BMI270 int16 samples, safely inside int32 bounds.
    return {x, y, z};
}
}
