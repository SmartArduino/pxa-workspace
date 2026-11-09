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
    for (int gestures = 0; gestures <= 1; ++gestures) for (unsigned i = 0; i < sizeof(modes); ++i) {
        pxa_window_snapshot_t snapshot = board;
        configuration.status_bar_mode = modes[i];
        configuration.navigation_bar_mode = PXA_WINDOW_BAR_HIDDEN;
        pxa_host_window_apply_bar_mode(&snapshot, &configuration, gestures);
        assert(snapshot.system_bar_insets.top ==
               (modes[i] == PXA_WINDOW_BAR_VISIBLE ? 28u : 0u));
        assert(snapshot.system_bar_insets.left == 26);
        assert(snapshot.system_bar_insets.bottom == (gestures ? 20u : 0u));
        assert(snapshot.safe_insets.top == 8 && snapshot.safe_insets.left == 10);
    }
    uint32_t bars[4], gestures[4];
    pxa_host_window_split_ui_insets(&board.system_bar_insets, bars, gestures, 1, 1);
    assert(bars[0] == 28 && bars[2] == 0 && bars[3] == 0);
    assert(gestures[2] == 20 && gestures[3] == 26);
    pxa_host_window_split_ui_insets(&board.system_bar_insets, bars, gestures, 0, 0);
    assert(bars[0] == 28 && bars[2] == 20 && bars[3] == 26);
    assert(gestures[0] == 0 && gestures[1] == 0 && gestures[2] == 0 && gestures[3] == 0);
    puts("Window metrics: hidden status bar releases space; safe/gesture insets survive");
    return 0;
}
