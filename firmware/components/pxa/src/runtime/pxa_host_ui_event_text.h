#ifndef PXA_HOST_UI_EVENT_TEXT_H
#define PXA_HOST_UI_EVENT_TEXT_H

#include "pxa_host_command.h"
#include "pxa/ui.h"
#include <stddef.h>
#include <string.h>

/* Ownership moves with the queued command. Only long text allocates; the
 * command union is still bounded by its existing 130-byte identity payload. */
static inline int pxa_host_ui_event_set_text(
    pxa_host_ui_event_t *event, const void *text, size_t size,
    void *(*allocate)(size_t)) {
    if (event == NULL || event->text_heap != NULL || event->text_size != 0 ||
        (text == NULL && size != 0) || size > PXA_UI_EVENT_TEXT_MAX_BYTES)
        return 0;
    if (size > sizeof(event->text)) {
        if (allocate == NULL) return 0;
        event->text_heap = allocate(size);
        if (event->text_heap == NULL) return 0;
        memcpy(event->text_heap, text, size);
    } else if (size != 0) memcpy(event->text, text, size);
    event->text_size = (uint16_t)size;
    return 1;
}

static inline const void *pxa_host_ui_event_text(const pxa_host_ui_event_t *event) {
    return event->text_heap != NULL ? event->text_heap : event->text;
}

static inline void pxa_host_ui_event_release_text(
    pxa_host_ui_event_t *event, void (*release)(void *)) {
    if (event->text_heap != NULL) release(event->text_heap);
    event->text_heap = NULL;
    event->text_size = 0;
}
#endif
