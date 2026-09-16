#ifndef PXA_ESP_NET_H
#define PXA_ESP_NET_H

#include <stddef.h>
#include <stdint.h>

#include "pxa/net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_ESP_NET_MAX_PENDING UINT16_C(4)
#define PXA_ESP_NET_MAX_RESPONSE_BYTES UINT32_C(4096)
#define PXA_ESP_NET_MAX_INLINE_BODY_BYTES UINT32_C(2048)
#define PXA_ESP_NET_REQUEST_HEADER_BYTES UINT32_C(1024)
#define PXA_ESP_NET_RESPONSE_HEADER_BYTES UINT32_C(1024)
#define PXA_ESP_NET_DEFAULT_TIMEOUT_MS UINT32_C(15000)

typedef void (*pxa_esp_net_notify_fn)(void *context);

typedef struct {
    uint64_t started_requests;
    uint64_t completed_requests;
    uint64_t cancelled_requests;
    uint64_t timed_out_requests;
    uint64_t queue_full_requests;
    uint64_t response_bytes;
    uint32_t peak_response_bytes;
    uint32_t slot_storage_bytes;
    uint16_t active_slots;
    uint16_t peak_active_slots;
} pxa_esp_net_snapshot_t;

int pxa_esp_net_backend(pxa_net_backend_t *output,
                        pxa_esp_net_notify_fn notify, void *notify_context);
void pxa_esp_net_reset_requests(void);
void pxa_esp_net_snapshot(pxa_esp_net_snapshot_t *output);

#ifdef __cplusplus
}
#endif

#endif
