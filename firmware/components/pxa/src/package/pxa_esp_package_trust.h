#ifndef PXA_ESP_PACKAGE_TRUST_H
#define PXA_ESP_PACKAGE_TRUST_H
#include "pxa/posix/pxa_posix_installer.h"
#include "pxa/esp/pxa_esp_mbedtls.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Immutable product trust, independent of filesystem mounts, package catalogs
 * and per-app registries. Caller sets destination, space and layout callbacks.
 * Anchors stay alive for the process lifetime. Each operation owner supplies
 * its own verifier context: embedded SPKI pointers are transient state. */
pxa_status_t pxa_esp_package_installer_profile(pxa_posix_installer_config_t *,
    pxa_esp_mbedtls_trust_t *verification_context);
#ifdef __cplusplus
}
#endif
#endif
