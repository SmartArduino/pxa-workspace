#pragma once
#include <algorithm>
#include <cstring>
#include <pxa/pxa_esp_surface.h>

namespace pxa::board {
// Copy a clipped span with independent source/destination pitches. Packed
// spans can use one bulk copy across rows.
template<auto BulkCopy = std::memcpy>
inline void CopyRgb565Rows(uint16_t* destination, size_t destination_stride,
                           const uint8_t* source, size_t source_stride_bytes,
                           size_t width, size_t height) {
    const size_t row_bytes = width * sizeof(uint16_t);
    if (destination_stride == width && source_stride_bytes == row_bytes) {
        BulkCopy(destination, source, row_bytes * height);
        return;
    }
    for (size_t row = 0; row < height; ++row)
        std::memcpy(destination + row * destination_stride,
                    source + row * source_stride_bytes, row_bytes);
}

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
        // A small overlay at each end of the display can produce a plane
        // spanning the whole screen. Skip transparent runs four pixels at
        // a time, without touching either RGB565 buffer in those runs.
        auto blend = [](uint16_t* dst, const uint8_t* src, uint8_t a, uint8_t opacity) {
            if (!a) return;
            uint16_t foreground;
            std::memcpy(&foreground, src, 2);
            *dst = pxa_esp_surface_blend_alpha_pixel(*dst, foreground, a, opacity);
        };
        int count = right - left;
        for (; count && (reinterpret_cast<uintptr_t>(alpha) & 3u);
             --count, source += 2, ++alpha, ++destination)
            blend(destination, source, *alpha, plane.opacity);
        for (; count >= 4; count -= 4, source += 8, alpha += 4, destination += 4) {
            uint32_t mask;
            std::memcpy(&mask, __builtin_assume_aligned(alpha, 4), sizeof(mask));
            if (mask)
                for (int i = 0; i < 4; ++i)
                    blend(destination + i, source + i * 2, alpha[i], plane.opacity);
        }
        for (; count; --count, source += 2, ++alpha, ++destination)
            blend(destination, source, *alpha, plane.opacity);
    }
}

// Fast path for an opaque, unscaled RGB565 Surface. Copy visible row spans,
// then visit alpha pixels only where the guest/system planes actually overlap.
// Region masks and scaled/ARGB Surfaces retain the board's scalar fallback.
template<auto BulkCopy = std::memcpy>
inline bool ComposeRgb565Rows(const pxa_esp_surface_frame_t& frame,
                             uint16_t* pixels, int x, int y, int width, int height,
                             size_t stride, int origin_x, int origin_y, int scale) {
    if (frame.format != PXA_SURFACE_FORMAT_RGB565 || scale != 1 || frame.opaque_ui_region_count)
        return false;
    const int left = std::max(x, origin_x), top = std::max(y, origin_y);
    const int right = std::min(int64_t(x) + width, int64_t(origin_x) + frame.width);
    const int bottom = std::min(int64_t(y) + height, int64_t(origin_y) + frame.height);
    if (left < right && top < bottom) {
        auto* destination = pixels + size_t(top - y) * stride + left - x;
        const auto* source = frame.pixels + size_t(top - origin_y) * frame.stride_bytes
            + size_t(left - origin_x) * sizeof(uint16_t);
        CopyRgb565Rows<BulkCopy>(destination, stride, source, frame.stride_bytes,
                       right - left, bottom - top);
    }
    if (!frame.suppress_guest_alpha)
        BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.ui_alpha_plane);
    BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.system_alpha_plane);
    return true;
}
}
