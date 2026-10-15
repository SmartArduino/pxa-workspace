#include "pxa_host_window_metrics.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* The board reports a 28 px status bar and persistent system gestures.
     * Switching to fullscreen must release the top, including on refresh;
     * returning to ordinary chrome must restore the board's reservation. */
    const pxa_window_snapshot_t board = {
        .safe_insets = {.top = 8, .right = 10, .bottom = 8, .left = 10},
        .system_bar_insets = {.top = 28, .left = 26, .bottom = 20},
    };
    pxa_window_configuration_t configuration = {0};
    const uint8_t modes[] = {PXA_WINDOW_BAR_VISIBLE, PXA_WINDOW_BAR_HIDDEN,
                            PXA_WINDOW_BAR_HIDDEN, PXA_WINDOW_BAR_TRANSIENT,
                            PXA_WINDOW_BAR_VISIBLE};
    for (unsigned i = 0; i < sizeof(modes); ++i) {
        pxa_window_snapshot_t snapshot = board;
        configuration.status_bar_mode = modes[i];
        configuration.navigation_bar_mode = PXA_WINDOW_BAR_HIDDEN;
        pxa_host_window_apply_bar_mode(&snapshot, &configuration);
        assert(snapshot.system_bar_insets.top ==
               (modes[i] == PXA_WINDOW_BAR_HIDDEN ? 0u : 28u));
        assert(snapshot.system_bar_insets.left == 26);
        assert(snapshot.system_bar_insets.bottom == 20);
        assert(snapshot.safe_insets.top == 8 && snapshot.safe_insets.left == 10);
    }
    puts("Window metrics: hidden status bar releases space; safe/gesture insets survive");
    return 0;
}
