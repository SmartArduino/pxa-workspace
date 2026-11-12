// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace espp4_a {
constexpr unsigned kCameraPreviewWidth = 320, kCameraPreviewHeight = 180;
// PPA uses 1/16 scale steps: 7/16 fills the 480x854 panel without CPU enlargement.
constexpr unsigned kCameraPpaPreviewWidth = 840, kCameraPpaPreviewHeight = 472;
constexpr size_t kCameraPpaPreviewBytes = size_t(kCameraPpaPreviewWidth) * kCameraPpaPreviewHeight * 2;
constexpr bool CameraPortrait(unsigned width, unsigned height) { return height > width; }
constexpr bool CameraNativePixels(unsigned width, unsigned height) {
    return (width == 1920 && height == 1080) || (width == 1080 && height == 1920);
}
constexpr unsigned CameraCropWidth(unsigned width, unsigned height) {
    return CameraPortrait(width, height) ? 1080u * height / width : 1080u * width / height;
}
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
            for (unsigned x = 0; x < width; ++x)
                out[x] = row[columns ? columns[x] : uint64_t(x) * source_width / width];
        }
        source_y += row_step; error += remainder;
        if (error >= height) { ++source_y; error -= height; }
    }
}
// Resize in sensor coordinates, then turn clockwise. No full-size temporary
// image is needed by the CPU fallback or smaller portrait profiles.
inline void CameraResizeOriented(const uint16_t* source, unsigned source_width, unsigned source_height,
                                 size_t source_stride, uint16_t* target, unsigned width,
                                 unsigned height, const uint16_t* columns, bool portrait) {
    if (!portrait) {
        CameraResize(source, source_width, source_height, source_stride, target, width, height, columns);
        return;
    }
    for (unsigned y = 0; y < height; ++y) {
        const unsigned source_x = columns ? columns[y] : uint64_t(y) * source_width / height;
        auto* row = target + size_t(y) * width;
        for (unsigned x = 0; x < width; ++x)
            row[x] = source[size_t(uint64_t(width - 1 - x) * source_height / width) * source_stride + source_x];
    }
}
}
