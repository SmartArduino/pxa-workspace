#ifndef PXA_HOST_WINDOW_METRICS_H
#define PXA_HOST_WINDOW_METRICS_H
#include "pxa/window.h"

/* Board insets include the status bar's usual height. A hidden status bar
 * occupies no space. Left/back and bottom/home gesture strips still receive
 * input even when their handles are hidden, so preserve those exclusions. */
static inline void pxa_host_window_apply_bar_mode(
    pxa_window_snapshot_t *snapshot,
    const pxa_window_configuration_t *configuration) {
    if (configuration->status_bar_mode == PXA_WINDOW_BAR_HIDDEN)
        snapshot->system_bar_insets.top = 0;
}
#endif
