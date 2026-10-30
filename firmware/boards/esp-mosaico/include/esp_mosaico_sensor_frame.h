#pragma once
#include <array>
#include <cstdint>

namespace mosaico_board {
// Published device frame with the panel in its normal orientation:
// +X toward the top edge, +Y toward the left edge, +Z out of the screen.
// BMI270 chip -> device is a proper 90-degree rotation, not a reflection.
// Apply identically to acceleration and angular velocity; preserve +Z and
// right-hand rotations so vector fusion/cross products remain consistent.
constexpr std::array<int32_t, 3> ImuDeviceFrame(int32_t x, int32_t y, int32_t z) {
    // Inputs are scaled BMI270 int16 samples, safely inside int32 bounds.
    return {y, -x, z};
}
}
