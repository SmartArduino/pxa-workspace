// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <lvgl.h>

namespace espp4_a {
struct ScanRows {
    int first = 0, end = 0;
    void Include(int top, int bottom) {
        if (top >= bottom) return;
        first = end ? std::min(first, top) : top;
        end = std::max(end, bottom);
    }
};
// The board drains its pending DMA fences before changing buffers/mode. Native
// LVGL retains clean pixels in DIRECT mode; guest composition requires FULL
// redraws so LVGL never reuses pixels written by the guest compositor.
class UiRenderMode {
public:
    bool direct() const { return direct_; }
    bool Desired(bool enabled, bool guest_visible, uint8_t rotation) const {
        return enabled && !guest_visible && !rotation && !full_requested_;
    }
    bool NeedsChange(bool desired) const { return direct_ != desired; }
    void RequireFull() { full_requested_ = true; }
    void Configure(lv_display_t* display, void* writable, void* scanning,
                   size_t bytes, bool direct) {
        lv_display_set_buffers(display, writable, scanning, bytes,
            direct ? LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_FULL);
        direct_ = direct;
        current_rows_ = {};
        previous_rows_ = {0, lv_display_get_vertical_resolution(display)};
        full_requested_ = false;
        overlay_drawn_ = false;
        // This first frame also removes any guest pixels retained in either
        // buffer when returning to the shell. LVGL synchronizes dirty areas.
        lv_obj_invalidate(lv_display_get_screen_active(display));
    }
    bool ShouldSubmit(lv_display_t* display) const {
        // DIRECT can flush several disjoint dirty rectangles for one buffer.
        // Only the last rectangle represents a complete image to scan out.
        return !direct_ || lv_display_flush_is_last(display);
    }
    void NoteFlushedArea(const lv_area_t* area) {
        if (direct_ && area) current_rows_.Include(area->y1, area->y2 + 1);
    }
    ScanRows TakeScanRows(int height) {
        // LVGL copies the previous render's damage into the other DPI buffer
        // before drawing this render's invalid areas. Both writes need sync.
        ScanRows result = previous_rows_;
        result.Include(current_rows_.first, current_rows_.end);
        previous_rows_ = current_rows_;
        current_rows_ = {};
        if (result.first < 0 || result.first >= result.end || result.end > height)
            return {0, height};
        return result;
    }
    void IncludeOverlay(lv_area_t* dirty, bool visible, const lv_area_t& bounds) const {
        // Raw diagnostic pixels must be repainted by LVGL before reuse or
        // synchronization. Include the previous overlay once when hiding it.
        if (direct_ && dirty && (visible || overlay_drawn_)) {
            dirty->x1 = std::min(dirty->x1, bounds.x1);
            dirty->y1 = std::min(dirty->y1, bounds.y1);
            dirty->x2 = std::max(dirty->x2, bounds.x2);
            dirty->y2 = std::max(dirty->y2, bounds.y2);
        }
    }
    void OverlayDrawn(bool visible) { overlay_drawn_ = visible; }
private:
    ScanRows previous_rows_, current_rows_;
    bool direct_ = false;
    bool full_requested_ = false;
    bool overlay_drawn_ = false;
};
} // namespace espp4_a
