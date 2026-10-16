#pragma once

#include <cstddef>
#include <cstdint>

#include <lvgl.h>
#include <pxa/pxa_esp_surface.h>

// PXA Surface presentation for the SenseCAP Watcher QSPI panel.
//
// The existing presenter rasterizes outside the LVGL lock, then presents an
// eligible fullscreen RGB565 frame through the board callback. Smaller
// surfaces and overlays without a complete plane use LVGL composition.
// Native scanout holds its lease until DMA completes; composed paths release
// after copying into their immutable output buffer.
namespace watcher_pxa_surface {

// Installs the Surface frame-ready callback and the presenter task. Returns
// false when the presenter task cannot be created; the board then keeps its
// plain LVGL flush path.
using DirectPresenter = bool (*)(void* context);
bool Install(lv_display_t* display, DirectPresenter presenter = nullptr,
             void* context = nullptr);

// Composites the latest Surface frame into one LVGL flush area. It must run on
// the LVGL task, after LVGL rendered the area and before the area is handed to
// the panel, so the alpha plane and the Surface lease stay immutable for the
// whole copy.
struct ComposedFrameInfo {
    uint64_t frame_id = 0;
    uint64_t input_timestamp_us = 0;
    uint32_t raster_ready_tick_us = 0;
};
ComposedFrameInfo ComposeFlushArea(const lv_area_t* area, uint8_t* pixels);
bool DirectFrameEligible(const pxa_esp_surface_frame_t& frame);
ComposedFrameInfo ComposeFrame(const pxa_esp_surface_frame_t& frame,
                              const lv_area_t* area, uint8_t* pixels);

// Packs a contiguous native RGB565 range into DMA SRAM in panel byte order.
// Offset and size are even, destination is uint16_t-aligned; unlike ComposeFrame
// this raw-pixel operation does not depend on Surface visibility or UI metadata.
void PackNativeRgb565Band(const void* source, void* destination,
                         size_t offset_bytes, size_t size_bytes);

}  // namespace watcher_pxa_surface
