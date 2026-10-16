#ifndef PXA_ESP_STORE_POLICY_H
#define PXA_ESP_STORE_POLICY_H

#include <stdbool.h>
#include <string.h>

#include "pxa_esp_package_store.h"

/* Service 20 is available to active installed apps. The Host keeps package
 * authentication and install/uninstall confirmation. The source location,
 * publisher and app name do not grant or remove this capability. */
static inline bool pxa_esp_store_installer_authorized(
    const pxa_esp_package_record_t *record, const char *active_identity) {
    return record != NULL && active_identity != NULL &&
           strcmp(record->id, active_identity) == 0 &&
           record->installed && record->enabled;
}

#endif
