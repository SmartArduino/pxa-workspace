#include "pxa.h"
#include "pxa_event_port.h"
#include "pxa_ui.h"
#include "pxa_window.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t events[PXA_MAX_CONTROL_BYTES], packet[512];
static unsigned pointers, controllers, transitions, heartbeats, background;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (unsigned)t.tv_nsec / 1000;
}

static void note(const char *kind, int value) {
    char message[160];
    snprintf(message, sizeof(message), "MAINLOOP %s value=%d heartbeat=%u pointer=%u controller=%u lifecycle=%u background=%u",
             kind, value, heartbeats, pointers, controllers, transitions, background);
    pxa_log_write(2, message);
}

static void dispatch(const pxa_event_t *event) {
    uint8_t state;
    pxa_ui_pointer_data_t pointer;
    pxa_ui_controller_data_t controller;
    if (pxa_lifecycle_parse(event, &state)) {
        background = state == PXA_SYSTEM_LIFECYCLE_BACKGROUND;
        ++transitions; note("lifecycle", state);
    } else if (pxa_ui_parse_pointer(event, &pointer)) {
        ++pointers; note("pointer", pointer.phase);
    } else if (pxa_ui_parse_controller(event, &controller)) {
        ++controllers; note("controller", (int)controller.buttons);
    }
}

int32_t pxa_app_start(const uint8_t *config, uint32_t size) {
    (void)config; (void)size;
    pxa_ui_transaction_t tx = {0};
    if (!pxa_ui_transaction_begin(&tx, 1, PXA_UI_TRANSACTION_REPLACE_SURFACE, packet, sizeof(packet)) ||
        !pxa_ui_create(&tx, 1, 0, 0, PXA_UI_NODE_ROOT) ||
        !pxa_ui_set_rgba(&tx, 1, PXA_UI_PROPERTY_BACKGROUND, 0x16445aff) ||
        !pxa_ui_set_event_mask(&tx, 1, PXA_UI_EVENT_MASK_POINTER | PXA_UI_EVENT_MASK_CONTROLLER_STATE) ||
        !pxa_ui_create(&tx, 2, 1, 0, PXA_UI_NODE_TEXT) ||
        !pxa_ui_set_text(&tx, 2, "Core 2.1 main loop", 18) ||
        !pxa_ui_transaction_commit(&tx)) return PXA_STATUS_INTERNAL;
    (void)pxa_window_fullscreen();
    note("start", 0);
    return pxa_event_port_open(31);
}

int32_t pxa_app_on_event(const uint8_t *data, uint32_t size) {
    pxa_event_t event;
    if (!pxa_parse_event(data, size, &event)) return PXA_STATUS_INTERNAL;
    pxa_event_port_result_t opened;
    if (!pxa_event_port_parse_open(&event, 31, &opened)) {
        dispatch(&event); return PXA_EVENT_HANDLED;
    }
    if (opened.status) return opened.status;
    /* This stack object must survive every native wait. The Host never calls
     * this export recursively while the retained call is running. */
    volatile uint8_t sentinel[128];
    for (unsigned i = 0; i < sizeof(sentinel); ++i) sentinel[i] = 0xa7;
    uint64_t next_heartbeat = now_us();
    note("entered", 0);
    for (;;) {
        int count = pxa_event_port_wait(opened.handle, events, sizeof(events), 100000);
        for (unsigned i = 0; i < sizeof(sentinel); ++i)
            if (sentinel[i] != 0xa7) return PXA_STATUS_INTERNAL;
        if (count > 0) {
            if (!pxa_parse_event(events, (unsigned)count, &event)) return PXA_STATUS_INTERNAL;
            dispatch(&event);
        } else if (count != PXA_STATUS_TIMED_OUT && count != PXA_STATUS_WOULD_BLOCK) {
            note("wait-error", count); return count;
        }
        uint64_t now = now_us();
        if (now >= next_heartbeat) {
            ++heartbeats; note("alive", 0); next_heartbeat = now + 1000000;
        }
    }
}

void pxa_app_stop(uint32_t reason) { (void)reason; }
