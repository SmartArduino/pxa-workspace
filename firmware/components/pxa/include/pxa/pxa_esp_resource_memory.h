#pragma once
#include "pxa/resource_budget.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Board startup only. Device allocators remain immutable across activations
 * and charge the same global budget as application resources. */
pxa_status_t pxa_esp_resource_memory_initialize(void);
const pxa_memory_allocator_t *pxa_esp_device_resource_allocator(unsigned memory_class, unsigned kind);
#ifdef __cplusplus
}
#endif
