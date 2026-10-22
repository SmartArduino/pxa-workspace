#pragma once
#include <algorithm>
#include <cstring>
#include <pxa/pxa_esp_surface.h>

namespace pxa::board {
// Blend only the intersection of an immutable alpha plane and an LVGL tile.
// Pixel strides may be padded; memcpy also accepts an unaligned source row.
inline void BlendRgb565PlaneRows(uint16_t* pixels, size_t stride,
                                int x, int y, int width, int height,
                                const pxa_esp_surface_ui_alpha_plane_t& plane) {
    if (!plane.visible || !plane.opacity || !plane.pixels || !plane.alpha) return;
    const int left = std::max<int32_t>(x, plane.x), top = std::max<int32_t>(y, plane.y);
    const int right = std::min(int64_t(x) + width, int64_t(plane.x) + plane.width);
    const int bottom = std::min(int64_t(y) + height, int64_t(plane.y) + plane.height);
    if (left >= right || top >= bottom) return;
    for (int row = top; row < bottom; ++row) {
        const auto* source = reinterpret_cast<const uint8_t*>(plane.pixels) +
            size_t(row - plane.y) * plane.pixel_stride_bytes + size_t(left - plane.x) * 2;
        const auto* alpha = plane.alpha + size_t(row - plane.y) * plane.alpha_stride_bytes + left - plane.x;
        auto* destination = pixels + size_t(row - y) * stride + left - x;
        for (int col = left; col < right; ++col, source += 2, ++alpha, ++destination) {
            if (!*alpha) continue;
            uint16_t foreground;
            std::memcpy(&foreground, source, 2);
            *destination = pxa_esp_surface_blend_alpha_pixel(*destination, foreground, *alpha, plane.opacity);
        }
    }
}

// Fast path for an opaque, unscaled RGB565 Surface. Copy visible row spans,
// then visit alpha pixels only where the guest/system planes actually overlap.
// Region masks and scaled/ARGB Surfaces retain the board's scalar fallback.
inline bool ComposeRgb565Rows(const pxa_esp_surface_frame_t& frame,
                             uint16_t* pixels, int x, int y, int width, int height,
                             size_t stride, int origin_x, int origin_y, int scale) {
    if (frame.format != PXA_SURFACE_FORMAT_RGB565 || scale != 1 || frame.opaque_ui_region_count)
        return false;
    const int left = std::max(x, origin_x), top = std::max(y, origin_y);
    const int right = std::min(int64_t(x) + width, int64_t(origin_x) + frame.width);
    const int bottom = std::min(int64_t(y) + height, int64_t(origin_y) + frame.height);
    if (left < right)
        for (int row = top; row < bottom; ++row)
            std::memcpy(pixels + size_t(row - y) * stride + left - x,
                frame.pixels + size_t(row - origin_y) * frame.stride_bytes + size_t(left - origin_x) * 2,
                size_t(right - left) * 2);
    if (!frame.suppress_guest_alpha)
        BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.ui_alpha_plane);
    BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.system_alpha_plane);
    return true;
}
}
