#ifndef PXA_ESP_PERMISSION_STORE_H
#define PXA_ESP_PERMISSION_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "pxa/permission.h"
#include "pxa/package.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32 Layer 2 permission persistence over the assets LittleFS partition.
 * Grants are kept in checksummed per-App A/B policy files; absent entries
 * default to deny. A bounded one-App verified snapshot cache avoids reading
 * both files for every declared tuple. Legacy NVS decisions migrate lazily. */

pxa_status_t pxa_esp_permission_store_load(
    void *context, pxa_bytes_t app_identity, pxa_bytes_t name,
    pxa_bytes_t scope, pxa_permission_decision_t *decision);
pxa_status_t pxa_esp_permission_store_save(
    void *context, pxa_bytes_t app_identity, pxa_bytes_t name,
    pxa_bytes_t scope, pxa_permission_decision_t decision);
pxa_status_t pxa_esp_permission_store_clear_app(
    pxa_bytes_t app_identity, const pxa_package_permission_t *permissions,
    size_t permission_count);

/* Populate a pxa_permission_store_t (context left NULL; the load/save
 * callbacks above are used directly). */
void pxa_esp_permission_store_bind(pxa_permission_store_t *store);

#ifdef __cplusplus
}
#endif

#endif
