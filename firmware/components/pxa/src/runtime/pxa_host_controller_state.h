#ifndef PXA_HOST_CONTROLLER_STATE_H
#define PXA_HOST_CONTROLLER_STATE_H

#include <stdint.h>

/* Owner-task cache of the last accepted state. This suppresses only identical
 * consecutive refreshes, never a press/release or a different controller.
 * A new UI generation or lifecycle transition requires a fresh delivery. */
typedef struct {
    uint64_t instance;
    uint32_t generation;
    uint16_t state;
    uint8_t controller;
    uint8_t valid;
} pxa_host_controller_state_t;

static inline int pxa_host_controller_state_matches(
    const pxa_host_controller_state_t *last, uint64_t instance,
    uint32_t generation, uint8_t controller, uint8_t connected,
    uint32_t buttons) {
    return last->valid && last->instance == instance &&
        last->generation == generation && last->controller == controller &&
        last->state == (uint16_t)(buttons | ((uint16_t)connected << 12));
}

static inline void pxa_host_controller_state_accept(
    pxa_host_controller_state_t *last, uint64_t instance,
    uint32_t generation, uint8_t controller, uint8_t connected,
    uint32_t buttons) {
    last->instance = instance;
    last->generation = generation;
    last->state = (uint16_t)(buttons | ((uint16_t)connected << 12));
    last->controller = controller;
    last->valid = 1;
}

#endif
