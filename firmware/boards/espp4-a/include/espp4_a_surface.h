// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_esp_surface.h>
namespace espp4_a {
// No retained trusted or guest alpha content may be omitted by the presenter.
bool IsOpaqueSurface(const pxa_esp_surface_frame_t&);
// Inspect the complete painted background, never just its corners. A direct
// letterboxed frame can replace it with a fill only while LVGL stays clean.
bool UniformBackground(const uint16_t* pixels, size_t count, uint16_t& color);
// Restore only the uncovered backdrop; Compose overwrites the opaque guest
// rectangle, including when its geometry changes between frames.
bool FillOpaqueBackdrop(const pxa_esp_surface_frame_t&, uint16_t* pixels,
                        int width, int height, uint16_t color);
// Safe to overwrite an entire logical buffer without repainting its LVGL
// background. Visibility/modal/fresh-frame gates are checked by the Host.
bool CoversDisplayOpaque(const pxa_esp_surface_frame_t&, int width, int height);
// Contiguous 1:1 pictures can be rotated directly, without an intermediate
// logical canvas. Trusted planes and masks always retain normal composition.
bool CanRotateSurfaceDirect(const pxa_esp_surface_frame_t&, int width, int height);
void OverlayRgb565RectToNative(const uint16_t* patch,uint16_t* output,
    int x,int y,unsigned width,unsigned height,unsigned native_width,
    unsigned native_height,uint8_t rotation);
void Compose(const pxa_esp_surface_frame_t&, uint16_t* pixels,
             int x, int y, int width, int height, size_t stride,
             int display_width = 480, int display_height = 854);
}
