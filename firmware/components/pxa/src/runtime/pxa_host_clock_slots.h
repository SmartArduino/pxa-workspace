#ifndef PXA_HOST_CLOCK_SLOTS_H
#define PXA_HOST_CLOCK_SLOTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_HOST_CLOCK_SLOT_COUNT 8u

typedef struct {
    uint64_t next_due_us;
    uint32_t component;
    uint32_t generation;
    uint16_t period_ms;
    uint8_t pending;
} pxa_host_clock_slot_t;

typedef struct {
    pxa_host_clock_slot_t slots[PXA_HOST_CLOCK_SLOT_COUNT];
} pxa_host_clock_slots_t;

typedef struct {
    uint32_t generation;
    uint8_t slot;
} pxa_host_clock_tick_t;

void pxa_host_clock_slots_init(pxa_host_clock_slots_t *slots);

int pxa_host_clock_slots_set(pxa_host_clock_slots_t *slots, uint8_t slot,
                             uint32_t component, uint16_t period_ms,
                             uint64_t now_us);

void pxa_host_clock_slots_clear_component(pxa_host_clock_slots_t *slots,
                                          uint32_t component);
void pxa_host_clock_slots_cancel_all(pxa_host_clock_slots_t *slots);

size_t pxa_host_clock_slots_take_due(pxa_host_clock_slots_t *slots,
                                     uint64_t now_us,
                                     pxa_host_clock_tick_t *ticks,
                                     size_t capacity);

int pxa_host_clock_slots_consume(pxa_host_clock_slots_t *slots, uint8_t slot,
                                 uint32_t generation,
                                 uint32_t *component);

void pxa_host_clock_slots_post_failed(pxa_host_clock_slots_t *slots,
                                      uint8_t slot, uint32_t generation);

#ifdef __cplusplus
}
#endif

#endif
