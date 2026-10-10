#include "pxa_host_ui_event_text.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

static size_t allocation_size, live;
static int fail;
static void *allocate(size_t size) {
    allocation_size = size;
    if (fail) return NULL;
    ++live; return malloc(size);
}
static void release(void *memory) { assert(live); --live; free(memory); }
int main(void) {
    pxa_host_ui_event_t input = {0}, queued;
    char text[PXA_UI_EVENT_TEXT_MAX_BYTES + 1];
    memset(text, 'x', sizeof(text));
    assert(sizeof(pxa_esp_host_command_t) == 144); /* Queue capacity stays unchanged. */
    assert(pxa_host_ui_event_set_text(&input, text, 64, allocate));
    assert(!live && !allocation_size && !memcmp(pxa_host_ui_event_text(&input), text, 64));
    pxa_host_ui_event_release_text(&input, release);
    assert(pxa_host_ui_event_set_text(&input, NULL, 0, allocate));
    for (size_t size = 65; size <= PXA_UI_EVENT_TEXT_MAX_BYTES; size += 13) {
        assert(pxa_host_ui_event_set_text(&input, text, size, allocate));
        assert(live == 1 && allocation_size == size && input.text_size == size);
        queued = input; memset(&input, 0, sizeof(input));
        assert(!memcmp(pxa_host_ui_event_text(&queued), text, size));
        pxa_host_ui_event_release_text(&queued, release); // Delivered or discarded as stale.
        pxa_host_ui_event_release_text(&queued, release);
        assert(!live);
    }
    fail = 1;
    assert(!pxa_host_ui_event_set_text(&input, text, 65, allocate));
    assert(!input.text_heap && !input.text_size && !live);
    fail = 0;
    assert(!pxa_host_ui_event_set_text(&input, text, sizeof(text), allocate));
    assert(pxa_host_ui_event_set_text(&input, text, 1000, allocate));
    pxa_host_ui_event_release_text(&input, release); // Failed queue insertion.
    assert(!live);
    puts("Dynamic UI text: exact allocations, inline short path, OOM and ownership cleanup OK");
}
