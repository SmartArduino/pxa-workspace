#ifndef PXA_HOST_MAINTENANCE_H
#define PXA_HOST_MAINTENANCE_H
#include "pxa_host_command.h"

/* One existing byte tracks queued, idempotent maintenance. Call under the
 * process-state lock; reliable input commands are never coalesced here. */
#define PXA_HOST_SCHEDULER_PENDING UINT8_C(128)
static inline uint8_t pxa_host_maintenance_bit(pxa_esp_host_command_type_t type) {
    switch (type) {
    case PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE: return 1;
    case PXA_ESP_HOST_CMD_NET_MAINTENANCE: return 2;
    case PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE: return 4;
    case PXA_ESP_HOST_CMD_LEASE_MAINTENANCE: return 8;
    default: return 0;
    }
}
static inline int pxa_host_maintenance_mark(uint8_t *pending,
                                           pxa_esp_host_command_type_t type) {
    const uint8_t bit = pxa_host_maintenance_bit(type);
    if (bit == 0 || (*pending & bit) != 0) return 0;
    *pending |= bit;
    return 1;
}
static inline void pxa_host_maintenance_clear(uint8_t *pending,
                                             pxa_esp_host_command_type_t type) {
    *pending &= (uint8_t)~pxa_host_maintenance_bit(type);
}
#endif
