#ifndef PXA_ESP_PACKAGE_POLICY_H
#define PXA_ESP_PACKAGE_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Package Store serializes access with its metadata lock. */
bool pxa_esp_package_policy_initialize(void);
void pxa_esp_package_policy_deinitialize(void);
bool pxa_esp_package_policy_is_disabled(const char *identity);
bool pxa_esp_package_policy_set_enabled(const char *identity, bool enabled);

#ifdef __cplusplus
}
#endif

#endif
