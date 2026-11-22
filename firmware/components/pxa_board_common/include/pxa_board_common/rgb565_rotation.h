#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pxa::board {
// Landscape orientation names follow LVGL: 270 maps logical (x,y) to native
// (height-1-y,x). The panel itself keeps MV/MX/MY clear in both orientations.
enum class LandscapeRotation { Deg270, Deg90 };

// Composite a logical RGB565 diagnostic patch into the physical output only.
// The retained logical frame stays clean when the overlay is later disabled.
inline void RotateRgb565Patch(const uint16_t* patch, size_t stride, uint16_t* native,
                             int width, int height, int x, int y, int patch_width,
                             int patch_height, LandscapeRotation rotation) {
    for (int row = 0; row < patch_height; ++row)
        for (int column = 0; column < patch_width; ++column) {
            const int lx = x + column, ly = y + row;
            if (lx < 0 || ly < 0 || lx >= width || ly >= height) continue;
            const int index = rotation == LandscapeRotation::Deg270
                ? lx * height + height - 1 - ly : (width - 1 - lx) * height + ly;
            native[index] = __builtin_bswap16(patch[row * stride + column]);
        }
}

// Copy a complete logical RGB565 frame into native portrait scan order and
// ST7789 wire byte order. Tiling keeps both PSRAM reads and writes local.
inline void RotateRgb565(const uint16_t* source, size_t source_stride,
                        uint16_t* native, int width, int height,
                        LandscapeRotation rotation) {
    constexpr int block = 16;
    alignas(16) uint16_t tile[block][block];
    for (int y0 = 0; y0 < height; y0 += block) {
        const int y1 = std::min(y0 + block, height);
        for (int x0 = 0; x0 < width; x0 += block) {
            const int x1 = std::min(x0 + block, width);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    tile[y - y0][x - x0] = __builtin_bswap16(source[y * source_stride + x]);
            for (int x = x0; x < x1; ++x) {
                if (rotation == LandscapeRotation::Deg270) {
                    auto* row = native + x * height + height - y1;
                    for (int y = y1 - 1; y >= y0; --y)
                        *row++ = tile[y - y0][x - x0];
                } else {
                    auto* row = native + (width - 1 - x) * height + y0;
                    for (int y = y0; y < y1; ++y)
                        *row++ = tile[y - y0][x - x0];
                }
            }
        }
    }
}

// Diagnostics read the completed physical output, rather than the LVGL source,
// and return it to callers in logical orientation and native CPU byte order.
inline void CaptureRotatedRgb565(const uint16_t* native, uint16_t* logical,
                                int width, int height, LandscapeRotation rotation) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int index = rotation == LandscapeRotation::Deg270
                ? x * height + height - 1 - y : (width - 1 - x) * height + y;
            logical[y * width + x] = __builtin_bswap16(native[index]);
        }
}

namespace detail {
// FixedScale is zero for the generic path. The common 2x and 4x paths can
// duplicate each pixel with fixed stores instead of a loop per pixel.
template<int FixedScale>
inline void RotateScaledRgb565Impl(const uint16_t* source, size_t source_stride,
                                  uint16_t* native, int width, int height, int dynamic_scale,
                                  LandscapeRotation rotation) {
    constexpr int block = 16;
    const int scale = FixedScale != 0 ? FixedScale : dynamic_scale;
    uint16_t tile[block][block];
    const bool reverse = rotation == LandscapeRotation::Deg270;
    const int source_step = reverse ? -1 : 1;
    const int output_height = height * scale;
    for (int y0 = 0; y0 < height; y0 += block) {
        const int y1 = std::min(y0 + block, height);
        const int tile_height = y1 - y0;
        const int first_source_row = reverse ? tile_height - 1 : 0;
        for (int x0 = 0; x0 < width; x0 += block) {
            const int x1 = std::min(x0 + block, width);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    tile[y - y0][x - x0] = __builtin_bswap16(source[y * source_stride + x]);
            for (int x = x0; x < x1; ++x) {
                for (int dx = 0; dx < scale; ++dx) {
                    const int column = x * scale + dx;
                    auto* row = reverse
                        ? native + column * output_height + (height - y1) * scale
                        : native + (width * scale - 1 - column) * output_height + y0 * scale;
                    int source_row = first_source_row;
                    for (int i = 0; i < tile_height; ++i, source_row += source_step) {
                        const auto color = tile[source_row][x - x0];
                        if constexpr (FixedScale == 0) {
                            for (int dy = 0; dy < scale; ++dy) row[dy] = color;
                        } else {
                            row[0] = row[1] = color;
                            if constexpr (FixedScale == 4) row[2] = row[3] = color;
                        }
                        row += scale;
                    }
                }
            }
        }
    }
}
}

// Integer scaling and rotation share one copy; no intermediate 320x240 frame.
inline void RotateScaledRgb565(const uint16_t* source, size_t source_stride,
                              uint16_t* native, int width, int height, int scale,
                              LandscapeRotation rotation) {
    if (scale == 1)
        RotateRgb565(source, source_stride, native, width, height, rotation);
    else if (scale == 2)
        detail::RotateScaledRgb565Impl<2>(source, source_stride, native, width, height, 2, rotation);
    else if (scale == 4)
        detail::RotateScaledRgb565Impl<4>(source, source_stride, native, width, height, 4, rotation);
    else
        detail::RotateScaledRgb565Impl<0>(source, source_stride, native, width, height, scale, rotation);
}
}
