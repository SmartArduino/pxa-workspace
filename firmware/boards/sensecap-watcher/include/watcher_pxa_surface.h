#pragma once

#include <cstdint>

#include <lvgl.h>

// PXA Surface presentation for the SenseCAP Watcher QSPI panel.
//
// The panel has no rotation and no TE synchronization, so unlike pai-touch the
// board does not need a direct-scanout rotation pipeline. The presenter task
// only wakes LVGL so the newly submitted Surface frame is rendered again, and
// the LVGL flush callback composites the latest Sensor Surface under the
// trusted UI alpha plane before the area is queued to the panel. The lease is
// released as soon as the composited copy is complete, so the Guest buffer
// returns to the Surface pool before the SPI transfer finishes.
namespace watcher_pxa_surface {

// Installs the Surface frame-ready callback and the presenter task. Returns
// false when the presenter task cannot be created; the board then keeps its
// plain LVGL flush path.
bool Install(lv_display_t* display);

// Composites the latest Surface frame into one LVGL flush area. It must run on
// the LVGL task, after LVGL rendered the area and before the area is handed to
// the panel, so the alpha plane and the Surface lease stay immutable for the
// whole copy.
void ComposeFlushArea(const lv_area_t* area, uint8_t* pixels);

}  // namespace watcher_pxa_surface
