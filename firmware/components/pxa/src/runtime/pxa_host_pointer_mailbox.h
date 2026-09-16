#ifndef PXA_HOST_POINTER_MAILBOX_H
#define PXA_HOST_POINTER_MAILBOX_H

#include <stdint.h>

#include "pxa_host_command.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_HOST_POINTER_MAILBOX_CAPACITY 8u
#define PXA_HOST_POINTER_MOVE_PHASE 1u

typedef struct {
    pxa_host_pointer_event_t events[PXA_HOST_POINTER_MAILBOX_CAPACITY];
    uint64_t last_move_us;
    uint8_t count;
} pxa_host_pointer_mailbox_t;

typedef char pxa_host_pointer_mailbox_size_must_not_exceed_336_bytes[
    sizeof(pxa_host_pointer_mailbox_t) <= 336u ? 1 : -1];

void pxa_host_pointer_mailbox_init(pxa_host_pointer_mailbox_t *mailbox);

int pxa_host_pointer_mailbox_push(pxa_host_pointer_mailbox_t *mailbox,
                                  const pxa_host_pointer_event_t *event);

int pxa_host_pointer_mailbox_take(pxa_host_pointer_mailbox_t *mailbox,
                                  uint64_t now_us,
                                  uint64_t move_min_interval_us,
                                  pxa_host_pointer_event_t *event);

int pxa_host_pointer_mailbox_has_move(
    const pxa_host_pointer_mailbox_t *mailbox);

int pxa_host_pointer_mailbox_move_is_due(
    const pxa_host_pointer_mailbox_t *mailbox, uint64_t now_us,
    uint64_t move_min_interval_us);

#ifdef __cplusplus
}
#endif

#endif
