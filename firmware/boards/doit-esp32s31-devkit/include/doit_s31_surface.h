// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_esp_surface.h>
namespace doit_s31 {
void Compose(const pxa_esp_surface_frame_t&, uint16_t* pixels,
             int x, int y, int width, int height, size_t stride);
}
