#ifndef PXA_ESP_STORE_POLICY_H
#define PXA_ESP_STORE_POLICY_H

#include <stdbool.h>
#include <string.h>

#include "pxa_esp_package_store.h"

/* Service 20 is an authority, not a normal manifest capability. The package
 * identity includes its signed publisher root, and the built-in bit is set
 * only by the firmware-owned package scan. */
static inline bool pxa_esp_store_installer_authorized(
    const pxa_esp_package_record_t *record, const char *active_identity) {
    return record != NULL && active_identity != NULL &&
           strcmp(record->id, active_identity) == 0 &&
           record->built_in && record->installed && record->enabled &&
           strcmp(record->app_id, "pxa-store") == 0;
}

#endif
