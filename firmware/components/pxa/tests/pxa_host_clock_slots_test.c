#include "pxa_host_clock_slots.h"

#include <assert.h>
#include <stdint.h>

static void test_due_ticks_are_coalesced(void) {
    pxa_host_clock_slots_t slots;
    pxa_host_clock_tick_t tick;
    uint32_t component = 0;
    pxa_host_clock_slots_init(&slots);
    assert(pxa_host_clock_slots_set(&slots, 3, 17, 20, 1000));
    assert(pxa_host_clock_slots_take_due(&slots, 20999, &tick, 1) == 0);
    assert(pxa_host_clock_slots_take_due(&slots, 21000, &tick, 1) == 1);
    assert(tick.slot == 3);
    assert(pxa_host_clock_slots_take_due(&slots, 61000, &tick, 1) == 0);
    assert(pxa_host_clock_slots_consume(&slots, tick.slot, tick.generation,
                                        &component));
    assert(component == 17);
    assert(pxa_host_clock_slots_take_due(&slots, 61000, &tick, 1) == 1);
    assert(pxa_host_clock_slots_consume(&slots, tick.slot, tick.generation,
                                        &component));
}

static void test_reconfiguration_invalidates_queued_tick(void) {
    pxa_host_clock_slots_t slots;
    pxa_host_clock_tick_t old_tick;
    pxa_host_clock_tick_t new_tick;
    uint32_t component = 0;
    pxa_host_clock_slots_init(&slots);
    assert(pxa_host_clock_slots_set(&slots, 1, 10, 16, 0));
    assert(pxa_host_clock_slots_take_due(&slots, 16000, &old_tick, 1) == 1);
    assert(pxa_host_clock_slots_set(&slots, 1, 11, 16, 16000));
    assert(!pxa_host_clock_slots_consume(&slots, old_tick.slot,
                                         old_tick.generation, &component));
    assert(pxa_host_clock_slots_take_due(&slots, 32000, &new_tick, 1) == 1);
    assert(pxa_host_clock_slots_consume(&slots, new_tick.slot,
                                        new_tick.generation, &component));
    assert(component == 11);
}

static void test_failed_post_can_be_retried(void) {
    pxa_host_clock_slots_t slots;
    pxa_host_clock_tick_t tick;
    pxa_host_clock_slots_init(&slots);
    assert(pxa_host_clock_slots_set(&slots, 0, 1, 16, 0));
    assert(pxa_host_clock_slots_take_due(&slots, 16000, &tick, 1) == 1);
    pxa_host_clock_slots_post_failed(&slots, tick.slot, tick.generation);
    assert(pxa_host_clock_slots_take_due(&slots, 32000, &tick, 1) == 1);
}

static void test_component_clear_invalidates_tick(void) {
    pxa_host_clock_slots_t slots;
    pxa_host_clock_tick_t tick;
    uint32_t component = 0;
    pxa_host_clock_slots_init(&slots);
    assert(pxa_host_clock_slots_set(&slots, 2, 42, 100, 0));
    assert(pxa_host_clock_slots_take_due(&slots, 100000, &tick, 1) == 1);
    pxa_host_clock_slots_clear_component(&slots, 42);
    assert(!pxa_host_clock_slots_consume(&slots, tick.slot, tick.generation,
                                         &component));
    assert(pxa_host_clock_slots_take_due(&slots, 1000000, &tick, 1) == 0);
}

int main(void) {
    test_due_ticks_are_coalesced();
    test_reconfiguration_invalidates_queued_tick();
    test_failed_post_can_be_retried();
    test_component_clear_invalidates_tick();
    return 0;
}
