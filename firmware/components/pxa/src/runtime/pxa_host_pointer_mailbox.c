#include "pxa_host_pointer_mailbox.h"

#include <stddef.h>
#include <string.h>

void pxa_host_pointer_mailbox_init(pxa_host_pointer_mailbox_t *mailbox) {
    if (mailbox == NULL) return;
    memset(mailbox, 0, sizeof(*mailbox));
}

int pxa_host_pointer_mailbox_push(pxa_host_pointer_mailbox_t *mailbox,
                                  const pxa_host_pointer_event_t *event) {
    uint8_t index;
    if (mailbox == NULL || event == NULL) return 0;

    if (event->phase == PXA_HOST_POINTER_MOVE_PHASE) {
        if (mailbox->count != 0 &&
            mailbox->events[mailbox->count - 1u].phase ==
                PXA_HOST_POINTER_MOVE_PHASE) {
            mailbox->events[mailbox->count - 1u] = *event;
            return 1;
        }
        if (mailbox->count < PXA_HOST_POINTER_MAILBOX_CAPACITY) {
            mailbox->events[mailbox->count++] = *event;
            return 1;
        }
        for (index = mailbox->count; index != 0; --index) {
            if (mailbox->events[index - 1u].phase ==
                PXA_HOST_POINTER_MOVE_PHASE) {
                mailbox->events[index - 1u] = *event;
                return 1;
            }
        }
        return 0;
    }

    if (mailbox->count == PXA_HOST_POINTER_MAILBOX_CAPACITY) {
        for (index = 0; index < mailbox->count; ++index) {
            if (mailbox->events[index].phase ==
                PXA_HOST_POINTER_MOVE_PHASE) {
                memmove(&mailbox->events[index], &mailbox->events[index + 1u],
                        (size_t)(mailbox->count - index - 1u) *
                            sizeof(mailbox->events[0]));
                --mailbox->count;
                break;
            }
        }
        if (mailbox->count == PXA_HOST_POINTER_MAILBOX_CAPACITY) return 0;
    }
    mailbox->events[mailbox->count++] = *event;
    return 1;
}

int pxa_host_pointer_mailbox_take(pxa_host_pointer_mailbox_t *mailbox,
                                  uint64_t now_us,
                                  uint64_t move_min_interval_us,
                                  pxa_host_pointer_event_t *event) {
    const pxa_host_pointer_event_t *next;
    int move_precedes_edge;
    int move_due;
    if (mailbox == NULL || event == NULL || mailbox->count == 0) return 0;

    next = &mailbox->events[0];
    move_precedes_edge =
        next->phase == PXA_HOST_POINTER_MOVE_PHASE && mailbox->count > 1u &&
        mailbox->events[1].phase != PXA_HOST_POINTER_MOVE_PHASE;
    move_due = next->phase != PXA_HOST_POINTER_MOVE_PHASE ||
               move_precedes_edge ||
               now_us - mailbox->last_move_us >= move_min_interval_us;
    if (!move_due) return 0;

    *event = *next;
    --mailbox->count;
    memmove(&mailbox->events[0], &mailbox->events[1],
            (size_t)mailbox->count * sizeof(mailbox->events[0]));
    if (event->phase == PXA_HOST_POINTER_MOVE_PHASE) {
        mailbox->last_move_us = now_us;
    }
    return 1;
}

int pxa_host_pointer_mailbox_has_move(
    const pxa_host_pointer_mailbox_t *mailbox) {
    uint8_t index;
    if (mailbox == NULL) return 0;
    for (index = 0; index < mailbox->count; ++index) {
        if (mailbox->events[index].phase == PXA_HOST_POINTER_MOVE_PHASE) {
            return 1;
        }
    }
    return 0;
}

int pxa_host_pointer_mailbox_move_is_due(
    const pxa_host_pointer_mailbox_t *mailbox, uint64_t now_us,
    uint64_t move_min_interval_us) {
    if (mailbox == NULL) return 0;
    return now_us - mailbox->last_move_us >= move_min_interval_us;
}
