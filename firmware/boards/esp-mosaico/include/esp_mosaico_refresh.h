#pragma once

#include <lvgl.h>
#include <initializer_list>
// The board is built/tested against its pinned LVGL. This internal operation
// clears only the invalidation queue; it does not change object/input state.
#include <src/core/lv_refr_private.h>
#include <src/display/lv_display_private.h>

namespace mosaico_board {

template <typename Present>
bool PresentAndAcknowledgeRefresh(lv_display_t* display, Present present) {
    if (!display) return false;
    // Match LVGL's layout pass before asking the existing trusted-alpha/direct
    // eligibility checks to compose a complete current frame.
    for (auto* root : {lv_display_get_screen_active(display), lv_display_get_screen_prev(display),
                      lv_display_get_layer_bottom(display), lv_display_get_layer_top(display),
                      lv_display_get_layer_sys(display)})
        if (root) lv_obj_update_layout(root);
    if (!present()) return false;
    // The submitted snapshot already contains the UI. Rendering the same
    // invalidations again copies/restores a full frame and blocks new guest
    // frames. This acknowledges only the current snapshot: future animation
    // ticks still invalidate normally. Every failed/fallback path is preserved.
    if (lv_display_is_invalidation_enabled(display))
        (void)lv_inv_area(display, nullptr);
    return true;
}

template <typename Present>
bool PresentPendingRefresh(lv_display_t* display, Present present) {
    // A presenter may have acknowledged the queue before its timer fires.
    // Avoid composing that same snapshot again at REFR_START in this case.
    return display && display->inv_p != 0 &&
        PresentAndAcknowledgeRefresh(display, present);
}

} // namespace mosaico_board
