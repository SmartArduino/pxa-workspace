#ifndef PXA_ESP_RESOURCE_POLICY_H
#define PXA_ESP_RESOURCE_POLICY_H
#include <stddef.h>
/* Quotas, not allocations. Preserve half of detected PSRAM outside the PXA
 * resource pool; actual allocations also preserve native-heap headroom. */
static inline void pxa_esp_resource_psram_limits(size_t total, int automatic,
    size_t configured, size_t configured_temporary, size_t *limit, size_t *temporary) {
    *limit = automatic ? total / 2u : configured;
    *temporary = automatic ? total / 4u : configured_temporary;
    if (*temporary > *limit) *temporary = *limit;
}
static inline size_t pxa_esp_resource_psram_available(size_t total, size_t free_bytes) {
    size_t reserve = total / 8u;
    if (reserve < 256u * 1024u) reserve = 256u * 1024u;
    return free_bytes > reserve ? free_bytes - reserve : 0;
}
#endif
