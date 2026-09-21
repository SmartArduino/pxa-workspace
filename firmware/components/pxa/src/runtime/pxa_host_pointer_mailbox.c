#include "pxa_host_pointer_mailbox.h"

#include <stddef.h>
#include <string.h>

static int same_pointer_stream(const pxa_host_pointer_event_t *left,
                               const pxa_host_pointer_event_t *right) {
    return left->id == right->id &&
           left->instance_id == right->instance_id &&
           left->surface == right->surface && left->node == right->node;
}

static pxa_host_pointer_move_clock_t *find_move_clock(
    pxa_host_pointer_mailbox_t *mailbox, uint8_t id, int create) {
    pxa_host_pointer_move_clock_t *available = NULL;
    uint8_t index;

    for (index = 0; index < PXA_HOST_POINTER_MOVE_CLOCK_CAPACITY; ++index) {
        pxa_host_pointer_move_clock_t *clock = &mailbox->move_clocks[index];
        if (clock->active && clock->id == id) return clock;
        if (!clock->active && available == NULL) available = clock;
    }
    if (create && available != NULL) {
        available->id = id;
        available->active = 1;
        available->last_move_us = 0;
        available->has_last_move = 0;
        return available;
    }
    return NULL;
}

static const pxa_host_pointer_move_clock_t *find_active_move_clock(
    const pxa_host_pointer_mailbox_t *mailbox, uint8_t id) {
    uint8_t index;

    for (index = 0; index < PXA_HOST_POINTER_MOVE_CLOCK_CAPACITY; ++index) {
        const pxa_host_pointer_move_clock_t *clock =
            &mailbox->move_clocks[index];
        if (clock->active && clock->id == id) return clock;
    }
    return NULL;
}

static void reset_move_clock(pxa_host_pointer_mailbox_t *mailbox,
                             uint8_t id) {
    pxa_host_pointer_move_clock_t *clock =
        find_move_clock(mailbox, id, 1);
    if (clock != NULL) {
        clock->last_move_us = 0;
        clock->has_last_move = 0;
    }
}

static void release_move_clock(pxa_host_pointer_mailbox_t *mailbox,
                               uint8_t id) {
    pxa_host_pointer_move_clock_t *clock =
        find_move_clock(mailbox, id, 0);
    if (clock != NULL) clock->active = 0;
}

static int move_is_due(const pxa_host_pointer_mailbox_t *mailbox,
                       const pxa_host_pointer_event_t *event,
                       uint64_t now_us, uint64_t move_min_interval_us) {
    const pxa_host_pointer_move_clock_t *clock =
        find_active_move_clock(mailbox, event->id);
    return clock == NULL || !clock->has_last_move ||
           now_us - clock->last_move_us >= move_min_interval_us;
}

void pxa_host_pointer_mailbox_init(pxa_host_pointer_mailbox_t *mailbox) {
    if (mailbox == NULL) return;
    memset(mailbox, 0, sizeof(*mailbox));
}

int pxa_host_pointer_mailbox_push(pxa_host_pointer_mailbox_t *mailbox,
                                  const pxa_host_pointer_event_t *event) {
    uint8_t index;
    if (mailbox == NULL || event == NULL) return 0;

    if (event->phase == PXA_HOST_POINTER_DOWN_PHASE) {
        reset_move_clock(mailbox, event->id);
    }

    if (event->phase == PXA_HOST_POINTER_MOVE_PHASE) {
        if (mailbox->count != 0 &&
            mailbox->events[mailbox->count - 1u].phase ==
                PXA_HOST_POINTER_MOVE_PHASE &&
            same_pointer_stream(&mailbox->events[mailbox->count - 1u],
                                event)) {
            mailbox->events[mailbox->count - 1u] = *event;
            return 1;
        }
        if (mailbox->count < PXA_HOST_POINTER_MAILBOX_CAPACITY) {
            mailbox->events[mailbox->count++] = *event;
            return 1;
        }
        for (index = mailbox->count; index != 0; --index) {
            if (mailbox->events[index - 1u].phase ==
                    PXA_HOST_POINTER_MOVE_PHASE &&
                same_pointer_stream(&mailbox->events[index - 1u], event)) {
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
    pxa_host_pointer_move_clock_t *clock;
    uint8_t index;
    if (mailbox == NULL || event == NULL || mailbox->count == 0) return 0;

    /* Preserve edge ordering. With only moves pending, each finger has its
     * own cadence so one active contact cannot throttle another. */
    for (index = 0; index < mailbox->count; ++index) {
        if (mailbox->events[index].phase != PXA_HOST_POINTER_MOVE_PHASE) {
            index = 0;
            break;
        }
    }
    if (index == mailbox->count) {
        for (index = 0; index < mailbox->count; ++index) {
            if (move_is_due(mailbox, &mailbox->events[index], now_us,
                            move_min_interval_us)) {
                break;
            }
        }
        if (index == mailbox->count) return 0;
    }

    *event = mailbox->events[index];
    memmove(&mailbox->events[index], &mailbox->events[index + 1u],
            (size_t)(mailbox->count - index - 1u) *
                sizeof(mailbox->events[0]));
    --mailbox->count;
    if (event->phase == PXA_HOST_POINTER_MOVE_PHASE) {
        clock = find_move_clock(mailbox, event->id, 1);
        if (clock != NULL) {
            clock->last_move_us = now_us;
            clock->has_last_move = 1;
        }
    } else if (event->phase != PXA_HOST_POINTER_DOWN_PHASE) {
        release_move_clock(mailbox, event->id);
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
    uint8_t index;
    if (mailbox == NULL) return 0;
    for (index = 0; index < mailbox->count; ++index) {
        if (mailbox->events[index].phase == PXA_HOST_POINTER_MOVE_PHASE &&
            move_is_due(mailbox, &mailbox->events[index], now_us,
                        move_min_interval_us)) {
            return 1;
        }
    }
    return 0;
}
