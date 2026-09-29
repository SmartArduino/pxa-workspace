#ifndef PXA_ESP_RESOURCE_MEMORY_H
#define PXA_ESP_RESOURCE_MEMORY_H
#include "pxa/resource_budget.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Boot/runtime thread only; call before starting any resource consumers.
 * Device descriptors survive app activations and share their global quota. */
pxa_status_t pxa_esp_resource_memory_initialize(void);
const pxa_memory_allocator_t *pxa_esp_device_resource_allocator(unsigned cls, unsigned kind);
/* One active package in this Host. Begin/end are runtime-thread operations.
 * Consumers hold charged storage through their final callback; end refuses
 * to recycle allocator descriptors until every old allocation has retired.
 * end() never waits: a retiring owner can overlap the next activation under
 * the global quota. Two fixed descriptor slots bound this overlap. */
pxa_status_t pxa_esp_resource_memory_begin(void);
pxa_status_t pxa_esp_resource_memory_end(void);
const pxa_memory_allocator_t *pxa_esp_resource_allocator(unsigned cls, unsigned kind);
void *pxa_esp_resource_allocate(unsigned cls, unsigned kind, size_t bytes);
void pxa_esp_resource_memory_stats(pxa_memory_stats_t *stats, size_t *fixed_bytes);
#ifdef __cplusplus
}
#endif
#endif
