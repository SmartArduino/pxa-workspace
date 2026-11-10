// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_surface.h"
#include <algorithm>
#include <cstring>
#include <array>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>
#include <pxa_board_common/surface_rgb565_rows.h>
namespace espp4_a {
using PixelPair = uint32_t __attribute__((may_alias));
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

// Camera/UI canvases are commonly full-size ARGB. Convert contiguous spans
// once, then composite trusted planes by rows instead of checking every plane
// and every opaque rectangle for each of the 409,920 panel pixels.
static bool ComposeArgbRows(const pxa_esp_surface_frame_t& frame, uint16_t* pixels,
                            int x, int y, int width, int height, size_t stride,
                            int origin_x, int origin_y, int scale) {
    if (frame.format != PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED || scale != 1 ||
        frame.opaque_ui_region_count > PXA_SURFACE_MAX_OPAQUE_UI_REGIONS) return false;
    const int left = std::max(x, origin_x), top = std::max(y, origin_y);
    const int right = std::min(x + width, origin_x + frame.width);
    const int bottom = std::min(y + height, origin_y + frame.height);
    std::array<std::pair<int, int>, PXA_SURFACE_MAX_OPAQUE_UI_REGIONS> masks;
    for (int row = top; row < bottom; ++row) {
        unsigned count = 0;
        for (unsigned i = 0; i < frame.opaque_ui_region_count; ++i) {
            const auto& r = frame.opaque_ui_regions[i];
            if (row < origin_y + r.y || row >= origin_y + r.y + r.height) continue;
            const int a = std::max(left, origin_x + r.x), b = std::min(right, origin_x + r.x + r.width);
            if (a < b) {
                unsigned at = count++;
                while (at && masks[at - 1].first > a) {
                    masks[at] = masks[at - 1]; --at;
                }
                masks[at] = {a, b};
            }
        }
        const auto convert = [&](int first, int last) {
            auto* out = pixels + size_t(row - y) * stride + first - x;
            const auto* in = frame.pixels + size_t(row - origin_y) * frame.stride_bytes + (first - origin_x) * 4;
            for (int column = first; column < last; ++column, in += 4, ++out) {
                uint32_t color; std::memcpy(&color, in, 4);
                *out = BlendArgb(*out, color);
            }
        };
        int column = left;
        for (unsigned i = 0; i < count; ++i) {
            if (column < masks[i].first) convert(column, masks[i].first);
            column = std::max(column, masks[i].second);
        }
        if (column < right) convert(column, right);
    }
    if (!frame.suppress_guest_alpha)
        pxa::board::BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.ui_alpha_plane);
    pxa::board::BlendRgb565PlaneRows(pixels, stride, x, y, width, height, frame.system_alpha_plane);
    return true;
}

// Copy/expand only spans not covered by trusted opaque UI. Native status and
// navigation bars must not force every scaled pixel through the scalar path.
static bool ComposeRgbRows(const pxa_esp_surface_frame_t& frame, uint16_t* pixels,
                          int x, int y, int width, int height, size_t stride,
                          int origin_x, int origin_y, int scale) {
    if (frame.format != PXA_SURFACE_FORMAT_RGB565 ||
        frame.opaque_ui_region_count > PXA_SURFACE_MAX_OPAQUE_UI_REGIONS) return false;
    const int left = std::max(x, origin_x), top = std::max(y, origin_y);
    const int right = std::min(x + width, origin_x + frame.width * scale);
    const int bottom = std::min(y + height, origin_y + frame.height * scale);
    std::array<std::pair<int, int>, PXA_SURFACE_MAX_OPAQUE_UI_REGIONS> masks;
    int previous_source_row = -1;
    unsigned mask_count = 0;
    if (left < right) for (int row = top; row < bottom; ++row) {
        const int source_row = (row - origin_y) / scale;
        const bool repeat_row = source_row == previous_source_row;
        if (!repeat_row) {
            mask_count = 0;
            for (unsigned i = 0; i < frame.opaque_ui_region_count; ++i) {
                const auto& r = frame.opaque_ui_regions[i];
                if (source_row < r.y || source_row >= r.y + r.height) continue;
                const int first = std::max(left, origin_x + r.x * scale);
                const int last = std::min(right, origin_x + (r.x + r.width) * scale);
                if (first >= last) continue;
                unsigned at = mask_count++;
                while (at && masks[at-1].first > first) {
                    masks[at] = masks[at-1]; --at;
                }
                masks[at] = {first, last};
            }
        }
        const auto expand = [&](int first, int last) {
            auto* destination = pixels + size_t(row-y) * stride + first-x;
            const auto* source = frame.pixels + size_t(source_row) * frame.stride_bytes;
            // Copy just the guest span. Native UI inside skipped masks can
            // differ on adjacent physical rows and must remain untouched.
            if (repeat_row) {
                std::memcpy(destination, destination-stride, size_t(last-first)*2);
            } else if (scale == 1) {
                std::memcpy(destination, source + size_t(first-origin_x)*2, size_t(last-first)*2);
            } else if (scale == 2 && !((first-origin_x)&1) && !((last-first)&1) &&
                       !(reinterpret_cast<uintptr_t>(destination)&3u)) {
                auto* pairs = reinterpret_cast<PixelPair*>(destination);
                source += size_t((first-origin_x)/2)*2;
                for (int pair=0; pair<(last-first)/2; ++pair) {
                    uint16_t color; std::memcpy(&color,source+pair*2,2);
                    pairs[pair] = uint32_t(color) * UINT32_C(0x00010001);
                }
            } else for (int column=first; column<last;) {
                const int offset = column-origin_x;
                const int count = std::min(scale-offset%scale, last-column);
                uint16_t color; std::memcpy(&color,source+size_t(offset/scale)*2,2);
                std::fill_n(destination,count,color);
                destination += count; column += count;
            }
        };
        int column = left;
        for (unsigned i=0; i<mask_count; ++i) {
            if (column < masks[i].first) expand(column,masks[i].first);
            column = std::max(column,masks[i].second);
        }
        if (column < right) expand(column,right);
        previous_source_row = source_row;
    }
    // Planes are applied after replication, preserving per-physical-row alpha.
    if (!frame.suppress_guest_alpha)
        pxa::board::BlendRgb565PlaneRows(pixels,stride,x,y,width,height,frame.ui_alpha_plane);
    pxa::board::BlendRgb565PlaneRows(pixels,stride,x,y,width,height,frame.system_alpha_plane);
    return true;
}

// Compose into the off-screen RGB565 DMA buffer using one immutable Surface.
void Compose(const pxa_esp_surface_frame_t& frame, uint16_t* pixels,
                    int tile_x, int tile_y, int width, int height, size_t stride,
                    int display_width, int display_height) {
    const size_t bpp = frame.format == PXA_SURFACE_FORMAT_RGB565 ? 2 : 4;
    if (frame.opaque_ui_region_count > PXA_SURFACE_MAX_OPAQUE_UI_REGIONS ||
        !pixels || !frame.visible || !frame.pixels || !frame.width || !frame.height ||
        stride < size_t(width) || frame.stride_bytes < frame.width * bpp ||
        (frame.format != PXA_SURFACE_FORMAT_RGB565 &&
         frame.format != PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED)) return;
    const int scale = std::max(1, int(pxa_surface_fit_scale(frame.width, frame.height, display_width, display_height)));
    int origin_x = frame.x, origin_y = frame.y;
    if (!origin_x && !origin_y && (frame.width * scale < display_width || frame.height * scale < display_height)) {
        origin_x = (display_width - frame.width * scale) / 2;
        origin_y = (display_height - frame.height * scale) / 2;
    }
    if (pxa::board::ComposeRgb565Rows(frame, pixels, tile_x, tile_y, width, height,
                                    stride, origin_x, origin_y, scale)) return;
    if (ComposeArgbRows(frame, pixels, tile_x, tile_y, width, height,
                        stride, origin_x, origin_y, scale)) return;
    if (ComposeRgbRows(frame, pixels, tile_x, tile_y, width, height,
                          stride, origin_x, origin_y, scale)) return;
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
