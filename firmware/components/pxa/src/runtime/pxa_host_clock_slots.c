#include "pxa_host_clock_slots.h"

#include <limits.h>
#include <string.h>

static void invalidate(pxa_host_clock_slot_t *slot) {
    slot->period_ms = 0;
    slot->pending = 0;
    slot->next_due_us = 0;
    slot->component = 0;
    slot->generation++;
}

void pxa_host_clock_slots_init(pxa_host_clock_slots_t *slots) {
    if (slots == NULL) return;
    memset(slots, 0, sizeof(*slots));
}

int pxa_host_clock_slots_set(pxa_host_clock_slots_t *slots, uint8_t slot,
                             uint32_t component, uint16_t period_ms,
                             uint64_t now_us) {
    pxa_host_clock_slot_t *entry;
    uint64_t period_us;
    if (slots == NULL || slot >= PXA_HOST_CLOCK_SLOT_COUNT ||
        (period_ms != 0 && (period_ms < 16 || period_ms > 1000))) {
        return 0;
    }
    entry = &slots->slots[slot];
    invalidate(entry);
    if (period_ms == 0) return 1;
    period_us = (uint64_t)period_ms * UINT64_C(1000);
    if (now_us > UINT64_MAX - period_us) return 0;
    entry->component = component;
    entry->period_ms = period_ms;
    entry->next_due_us = now_us + period_us;
    return 1;
}

void pxa_host_clock_slots_clear_component(pxa_host_clock_slots_t *slots,
                                          uint32_t component) {
    uint8_t index;
    if (slots == NULL) return;
    for (index = 0; index < PXA_HOST_CLOCK_SLOT_COUNT; ++index) {
        pxa_host_clock_slot_t *entry = &slots->slots[index];
        if (entry->period_ms != 0 && entry->component == component) {
            invalidate(entry);
        }
    }
}

void pxa_host_clock_slots_cancel_all(pxa_host_clock_slots_t *slots) {
    uint8_t index;
    if (slots == NULL) return;
    for (index = 0; index < PXA_HOST_CLOCK_SLOT_COUNT; ++index) {
        invalidate(&slots->slots[index]);
    }
}

size_t pxa_host_clock_slots_take_due(pxa_host_clock_slots_t *slots,
                                     uint64_t now_us,
                                     pxa_host_clock_tick_t *ticks,
                                     size_t capacity) {
    size_t count = 0;
    uint8_t index;
    if (slots == NULL || (ticks == NULL && capacity != 0)) return 0;
    for (index = 0;
         index < PXA_HOST_CLOCK_SLOT_COUNT && count < capacity; ++index) {
        pxa_host_clock_slot_t *entry = &slots->slots[index];
        uint64_t elapsed_us;
        uint64_t period_us;
        uint64_t periods;
        if (entry->period_ms == 0 || entry->pending ||
            now_us < entry->next_due_us) {
            continue;
        }
        period_us = (uint64_t)entry->period_ms * UINT64_C(1000);
        elapsed_us = now_us - entry->next_due_us;
        periods = elapsed_us / period_us + 1u;
        if (periods > (UINT64_MAX - entry->next_due_us) / period_us) {
            entry->next_due_us = UINT64_MAX;
        } else {
            entry->next_due_us += periods * period_us;
        }
        entry->pending = 1;
        ticks[count].slot = index;
        ticks[count].generation = entry->generation;
        count++;
    }
    return count;
}

int pxa_host_clock_slots_consume(pxa_host_clock_slots_t *slots, uint8_t slot,
                                 uint32_t generation,
                                 uint32_t *component) {
    pxa_host_clock_slot_t *entry;
    if (slots == NULL || component == NULL ||
        slot >= PXA_HOST_CLOCK_SLOT_COUNT) {
        return 0;
    }
    entry = &slots->slots[slot];
    if (!entry->pending || entry->generation != generation) return 0;
    entry->pending = 0;
    if (entry->period_ms == 0) return 0;
    *component = entry->component;
    return 1;
}

void pxa_host_clock_slots_post_failed(pxa_host_clock_slots_t *slots,
                                      uint8_t slot, uint32_t generation) {
    pxa_host_clock_slot_t *entry;
    if (slots == NULL || slot >= PXA_HOST_CLOCK_SLOT_COUNT) return;
    entry = &slots->slots[slot];
    if (entry->generation == generation) entry->pending = 0;
}
