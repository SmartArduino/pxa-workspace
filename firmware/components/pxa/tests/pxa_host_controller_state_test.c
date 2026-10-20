#include "pxa_host_controller_state.h"
#include <assert.h>

int main(void) {
    pxa_host_controller_state_t last = {0};
    assert(sizeof(last) == 16);
    for (unsigned id = 0; id < 256; ++id) {
        for (unsigned buttons = 0; buttons < 4096; ++buttons) {
            assert(!pxa_host_controller_state_matches(&last, 7, 3, id, 1, buttons));
            pxa_host_controller_state_accept(&last, 7, 3, id, 1, buttons);
            assert(pxa_host_controller_state_matches(&last, 7, 3, id, 1, buttons));
            assert(!pxa_host_controller_state_matches(&last, 8, 3, id, 1, buttons));
            assert(!pxa_host_controller_state_matches(&last, 7, 4, id, 1, buttons));
            assert(!pxa_host_controller_state_matches(&last, 7, 3, id, 0, 0));
        }
    }
    /* A failed downstream submission must not call accept: the previous
     * state still matches, while the new release remains eligible to retry. */
    assert(!pxa_host_controller_state_matches(&last, 7, 3, 255, 1, 0));
    pxa_host_controller_state_accept(&last, 7, 3, 255, 1, 0);
    assert(pxa_host_controller_state_matches(&last, 7, 3, 255, 1, 0));
    last.valid = 0; /* Foreground/background notifications release Guest keys. */
    assert(!pxa_host_controller_state_matches(&last, 7, 3, 255, 1, 0));
    return 0;
}
