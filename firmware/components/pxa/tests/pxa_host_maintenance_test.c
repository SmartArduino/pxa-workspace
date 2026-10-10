#include "pxa_host_maintenance.h"
#include <assert.h>
#include <stddef.h>

int main(void) {
    uint8_t pending = PXA_HOST_SCHEDULER_PENDING;
    const pxa_esp_host_command_type_t types[] = {
        PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE, PXA_ESP_HOST_CMD_NET_MAINTENANCE,
        PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE, PXA_ESP_HOST_CMD_LEASE_MAINTENANCE};
    unsigned queued = 0;
    /* A slow Guest can hold the runtime for many timer periods. Only four
     * queue entries accumulate, leaving the 16-entry queue room for input. */
    for (unsigned period = 0; period < 1000; ++period)
        for (size_t i = 0; i < 4; ++i)
            queued += (unsigned)pxa_host_maintenance_mark(&pending, types[i]);
    assert(queued == 4 && pending == (PXA_HOST_SCHEDULER_PENDING | 15));
    assert(!pxa_host_maintenance_mark(&pending, PXA_ESP_HOST_CMD_UI_EVENT));
    pxa_host_maintenance_clear(&pending, PXA_ESP_HOST_CMD_UI_EVENT);
    assert(pending == (PXA_HOST_SCHEDULER_PENDING | 15));
    for (size_t i = 0; i < 4; ++i) {
        pxa_host_maintenance_clear(&pending, types[i]);
        assert(pending & PXA_HOST_SCHEDULER_PENDING);
        assert(pxa_host_maintenance_mark(&pending, types[i]));
        assert(!pxa_host_maintenance_mark(&pending, types[i]));
        pxa_host_maintenance_clear(&pending, types[i]);
    }
    assert(pending == PXA_HOST_SCHEDULER_PENDING);
    /* Failed queue sends must clear their reservation and remain retryable. */
    assert(pxa_host_maintenance_mark(&pending, types[1]));
    pxa_host_maintenance_clear(&pending, types[1]);
    assert(pxa_host_maintenance_mark(&pending, types[1]));
}
