#ifndef PXA_RASTER_HANDOFF_H
#define PXA_RASTER_HANDOFF_H
#include <stdint.h>

/* One owner publishes immutable job fields before waking the worker. Either
 * core may claim an unstarted job. After a worker claim the owner must wait
 * for completion before reusing the fields or returning borrowed memory. */
typedef struct { uint32_t pending; } pxa_raster_handoff_t;

static inline void pxa_raster_handoff_publish(pxa_raster_handoff_t *handoff) {
    __atomic_store_n(&handoff->pending, 1u, __ATOMIC_RELEASE);
}

static inline int pxa_raster_handoff_claim(pxa_raster_handoff_t *handoff) {
    uint32_t expected = 1;
    return __atomic_compare_exchange_n(&handoff->pending, &expected, 0u,
        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
#endif
