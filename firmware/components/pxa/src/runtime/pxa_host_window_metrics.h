#ifndef PXA_HOST_WINDOW_METRICS_H
#define PXA_HOST_WINDOW_METRICS_H
#include "pxa/window.h"

/* Window 0.4 keeps its combined legacy exclusions. UI environment reports
 * visual chrome and gesture input separately so Back does not shift a page. */
static inline void pxa_host_window_split_ui_insets(
    const pxa_window_insets_t *combined, uint32_t bars[4], uint32_t gestures[4],
    int gesture_navigation, int back_gesture) {
    bars[0] = combined->top;
    bars[1] = combined->right;
    bars[2] = gesture_navigation ? 0 : combined->bottom;
    bars[3] = back_gesture ? 0 : combined->left;
    gestures[0] = gestures[1] = 0;
    gestures[2] = gesture_navigation ? combined->bottom : 0;
    gestures[3] = back_gesture ? combined->left : 0;
}

/* Hidden/transient status chrome occupies no fixed space. Preserve active
 * gesture exclusions, while hidden button navigation gives back its height. */
static inline void pxa_host_window_apply_bar_mode(
    pxa_window_snapshot_t *snapshot,
    const pxa_window_configuration_t *configuration, int reserve_navigation_input) {
    if (configuration->status_bar_mode != PXA_WINDOW_BAR_VISIBLE)
        snapshot->system_bar_insets.top = 0;
    if (!reserve_navigation_input && configuration->navigation_bar_mode != PXA_WINDOW_BAR_VISIBLE)
        snapshot->system_bar_insets.bottom = 0;
}
#endif
