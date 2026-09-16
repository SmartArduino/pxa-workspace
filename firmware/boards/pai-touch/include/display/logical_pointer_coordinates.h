#pragma once

#include <cstdint>

namespace display_input {

enum class Rotation : uint8_t {
    k0,
    k90,
    k180,
    k270,
};

struct Point {
    int32_t x;
    int32_t y;
};

inline Point LogicalToRaw(int32_t logical_x, int32_t logical_y,
                          int32_t original_width, int32_t original_height,
                          Rotation rotation) {
    switch (rotation) {
        case Rotation::k90:
            return {logical_y, original_height - logical_x - 1};
        case Rotation::k180:
            return {original_width - logical_x - 1,
                    original_height - logical_y - 1};
        case Rotation::k270:
            return {original_width - logical_y - 1, logical_x};
        default:
            return {logical_x, logical_y};
    }
}

}  // namespace display_input
