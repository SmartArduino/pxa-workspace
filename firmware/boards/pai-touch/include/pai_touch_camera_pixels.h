// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

// JPEG uses full-range BT.601 YCbCr. Keep preview conversion consistent with
// the JPEG encoder's native YUYV input, including clipping at the gamut edges.
inline uint16_t PaiTouchYuvRgb565(uint8_t y, uint8_t cb, uint8_t cr) {
    const int u = int(cb) - 128;
    const int v = int(cr) - 128;
    const int luma = int(y) * 256;
    const int red = std::clamp((luma + 359*v + 128) >> 8, 0, 255);
    const int green = std::clamp((luma - 88*u - 183*v + 128) >> 8, 0, 255);
    const int blue = std::clamp((luma + 454*u + 128) >> 8, 0, 255);
    return uint16_t(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3));
}

inline bool PaiTouchYuyvPreview(std::span<const uint8_t> source, unsigned width,
    unsigned height, unsigned divisor, std::span<uint16_t> output) {
    if (!width || (width & 1) || width > 4096 || !height || height > 4096 ||
        !divisor || width % divisor || height % divisor)
        return false;
    const uint64_t pixels = uint64_t(width) * height;
    const unsigned output_width = width / divisor;
    const unsigned output_height = height / divisor;
    if (pixels*2 > source.size() || uint64_t(output_width)*output_height > output.size()) return false;
    if (divisor == 1) {
        // Each YUYV pair shares chroma. Round its three RGB contributions once
        // instead of repeating the color matrix for both pixels.
        for (size_t i = 0; i < size_t(pixels); i += 2) {
            const auto* pair = source.data() + i*2;
            const int u = int(pair[1]) - 128;
            const int v = int(pair[3]) - 128;
            const int red = (359*v + 128) >> 8;
            const int green = (-88*u - 183*v + 128) >> 8;
            const int blue = (454*u + 128) >> 8;
            const auto pixel = [red, green, blue](int y) {
                const int r = std::clamp(y + red, 0, 255);
                const int g = std::clamp(y + green, 0, 255);
                const int b = std::clamp(y + blue, 0, 255);
                return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            };
            output[i] = pixel(pair[0]);
            output[i+1] = pixel(pair[2]);
        }
        return true;
    }
    for (unsigned y = 0; y < output_height; ++y) {
        const auto* row = source.data() + size_t(y)*divisor*width*2;
        for (unsigned x = 0; x < output_width; ++x) {
            const unsigned pixel = x*divisor;
            const unsigned pair = (pixel & ~1u)*2;
            output[size_t(y)*output_width+x] = PaiTouchYuvRgb565(row[pixel*2], row[pair+1], row[pair+3]);
        }
    }
    return true;
}
