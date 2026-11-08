// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace espp4_a {
constexpr unsigned kCameraPreviewWidth = 320, kCameraPreviewHeight = 180;
inline void CameraColumns(uint16_t* columns, unsigned source_width, unsigned width) {
    const unsigned step = source_width / width, remainder = source_width % width;
    unsigned position = 0, error = 0;
    for (unsigned x = 0; x < width; ++x) {
        columns[x] = position; position += step; error += remainder;
        if (error >= width) { ++position; error -= width; }
    }
}
inline void CameraResize(const uint16_t* source, unsigned source_width, unsigned source_height,
                         size_t source_stride, uint16_t* target, unsigned width,
                         unsigned height, const uint16_t* columns) {
    const unsigned step = source_width / width;
    const bool integer = source_width % width == 0;
    const unsigned row_step = source_height / height, remainder = source_height % height;
    unsigned source_y = 0, error = 0;
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = source + size_t(source_y) * source_stride;
        auto* out = target + size_t(y) * width;
        if (integer) {
            for (unsigned x = 0; x < width; ++x) out[x] = row[x * step];
        } else {
            for (unsigned x = 0; x < width; ++x) out[x] = row[columns[x]];
        }
        source_y += row_step; error += remainder;
        if (error >= height) { ++source_y; error -= height; }
    }
}
}
