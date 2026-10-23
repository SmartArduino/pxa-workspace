#pragma once
#include "epdiy_v7_config.h"
#include <algorithm>
#include <cstring>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>

namespace epdiy_v7 {
inline uint16_t BlendArgb(uint16_t background, uint32_t argb) {
    const unsigned alpha = argb >> 24;
    if (!alpha) return background;
    const uint16_t foreground = ((argb >> 8) & 0xf800) |
        ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x001f);
    if (alpha == 255) return foreground;
    const unsigned inverse = 255 - alpha;
    const unsigned red = (foreground >> 11) + (((background >> 11) * inverse + 128) >> 8);
    const unsigned green = ((foreground >> 5) & 63) + ((((background >> 5) & 63) * inverse + 128) >> 8);
    const unsigned blue = (foreground & 31) + (((background & 31) * inverse + 128) >> 8);
    return (red << 11) | (green << 5) | blue;
}

inline uint16_t BlendPlane(uint16_t background,
                           const pxa_esp_surface_ui_alpha_plane_t& plane, int x, int y) {
    if (!plane.visible || !plane.opacity || !plane.pixels || !plane.alpha ||
        x < plane.x || y < plane.y || x >= plane.x + plane.width || y >= plane.y + plane.height)
        return background;
    const auto* row = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(plane.pixels) + (y - plane.y) * plane.pixel_stride_bytes);
    return pxa_esp_surface_blend_alpha_pixel(background, row[x - plane.x],
        plane.alpha[(y - plane.y) * plane.alpha_stride_bytes + x - plane.x], plane.opacity);
}

// Compose into a native RGB565 LVGL tile before DMA byte swapping. One Surface
// lease spans the entire LVGL refresh so all partial tiles show the same frame.
inline void Compose(const pxa_esp_surface_frame_t& frame, uint16_t* pixels,
                    int tile_x, int tile_y, int width, int height, size_t stride) {
    const size_t bpp = frame.format == PXA_SURFACE_FORMAT_RGB565 ? 2 : 4;
    if (!pixels || !frame.visible || !frame.pixels || !frame.width || !frame.height ||
        stride < size_t(width) || frame.stride_bytes < frame.width * bpp ||
        (frame.format != PXA_SURFACE_FORMAT_RGB565 &&
         frame.format != PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED)) return;
    const int scale = std::max(1, int(pxa_surface_fit_scale(frame.width, frame.height, kWidth, kHeight)));
    int origin_x = frame.x, origin_y = frame.y;
    if (!origin_x && !origin_y && (frame.width * scale < kWidth || frame.height * scale < kHeight)) {
        origin_x = (kWidth - frame.width * scale) / 2;
        origin_y = (kHeight - frame.height * scale) / 2;
    }
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            const int x = tile_x + column, y = tile_y + row;
            uint16_t value = pixels[row * stride + column];
            if (x >= origin_x && y >= origin_y &&
                x < origin_x + frame.width * scale && y < origin_y + frame.height * scale) {
                const int sx = (x - origin_x) / scale, sy = (y - origin_y) / scale;
                bool opaque_ui = false;
                for (uint8_t i = 0; i < frame.opaque_ui_region_count; ++i) {
                    const auto& rect = frame.opaque_ui_regions[i];
                    if (sx >= rect.x && sy >= rect.y && sx < rect.x + rect.width && sy < rect.y + rect.height)
                        opaque_ui = true;
                }
                if (!opaque_ui) {
                    const auto* source = frame.pixels + sy * frame.stride_bytes + sx * bpp;
                    if (bpp == 2) value = uint16_t(source[0] | source[1] << 8);
                    else {
                        uint32_t argb;
                        std::memcpy(&argb, source, sizeof(argb));
                        value = BlendArgb(value, argb);
                    }
                }
            }
            if (!frame.suppress_guest_alpha) value = BlendPlane(value, frame.ui_alpha_plane, x, y);
            value = BlendPlane(value, frame.system_alpha_plane, x, y);
            pixels[row * stride + column] = value;
        }
    }
}
}
