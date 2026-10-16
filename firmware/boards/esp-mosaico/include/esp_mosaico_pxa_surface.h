#pragma once

#include <cstdint>
#include <lvgl.h>
#include <pxa/pxa_esp_surface.h>

namespace mosaico_pxa_surface {

using DirectPresenter = bool (*)(void* context);
// Native is LVGL/Guest RGB565; Panel is the CO5300 MSB-first wire order.
enum class PixelOrder { Native, Panel };
bool Install(lv_display_t* display, DirectPresenter presenter = nullptr,
             void* context = nullptr);
bool DirectFrameEligible(const pxa_esp_surface_frame_t& frame);
bool TryAcceleratedDirectFrame(const pxa_esp_surface_frame_t& frame, uint8_t* pixels);
// Both pointers own separate complete 480x480 frames, not compact patches.
bool CopyUiSnapshot(const lv_area_t& dirty, const uint8_t* pixels, uint8_t* snapshot);
uint32_t AcceleratedFillCount();
uint32_t RasterTimeUs();
uint64_t ComposeFrame(const pxa_esp_surface_frame_t& frame,
                      const lv_area_t* area, uint8_t* pixels,
                      uint32_t stride_bytes = 0, PixelOrder order = PixelOrder::Panel);
uint64_t ComposeFlushArea(const lv_area_t* area, uint8_t* pixels,
                          uint32_t stride_bytes = 0, PixelOrder order = PixelOrder::Panel);

}
