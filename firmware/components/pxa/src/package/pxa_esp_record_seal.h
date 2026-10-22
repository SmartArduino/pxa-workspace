#ifndef PXA_ESP_RECORD_SEAL_H
#define PXA_ESP_RECORD_SEAL_H
#include "pxa/wire.h"
#include "pxa/status.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PXA_ESP_RECORD_TAG_BYTES 32u
typedef pxa_status_t (*pxa_esp_record_entropy_fn)(void *,uint8_t key[32]);
/* Provision once at a product-controlled point with a cryptographic entropy
 * source ready. NULL loads an existing key without creating one. This avoids
 * assuming esp_random has sufficient entropy while RF/ADC sources are off. */
pxa_status_t pxa_esp_record_seal_initialize(pxa_esp_record_entropy_fn,void *);
/* Host-only authentication for mutable volume-local control records. Purpose
 * separates formats; binding includes volume, install identity and locator.
 * One device secret is retained in NVS, without any per-app NVS entries.
 * Do not seal Guest-selected bindings or treat a valid tag as signed code.
 * This authenticates contents/identity, not freshness of a replayed record. */
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,
    pxa_bytes_t record,uint8_t tag[PXA_ESP_RECORD_TAG_BYTES]);
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,
    pxa_bytes_t record,const uint8_t tag[PXA_ESP_RECORD_TAG_BYTES]);
#ifdef __cplusplus
}
#endif
#endif
