// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_esp_surface.h>
namespace espp4_a {
// Safe to overwrite an entire logical buffer without repainting its LVGL
// background. Visibility/modal/fresh-frame gates are checked by the Host.
bool CoversDisplayOpaque(const pxa_esp_surface_frame_t&, int width, int height);
void Compose(const pxa_esp_surface_frame_t&, uint16_t* pixels,
             int x, int y, int width, int height, size_t stride,
             int display_width = 480, int display_height = 854);
}
