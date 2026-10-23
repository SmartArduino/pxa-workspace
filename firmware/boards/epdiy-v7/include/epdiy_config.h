#pragma once
#include "sdkconfig.h"
#include "epdiy_refresh.h"
#include <pxa/pxa_esp_surface.h>
namespace epdiy_v7 {
#ifdef CONFIG_PXA_EPDIY_PORTRAIT
constexpr bool kPortrait = true;
#else
constexpr bool kPortrait = false;
#endif
constexpr int kWidth = kPortrait ? kNativeHeight : kNativeWidth;
constexpr int kHeight = kPortrait ? kNativeWidth : kNativeHeight;
// The connected 40-pin panel is mounted opposite the reference's reader view.
// Keep logical UI/input coordinates stable; rotate only the physical pixels.
constexpr bool kInverted = true;
void Compose(const pxa_esp_surface_frame_t& frame, uint16_t* pixels,
             int tile_x, int tile_y, int width, int height, size_t stride);
}
