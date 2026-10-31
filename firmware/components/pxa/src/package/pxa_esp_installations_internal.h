#ifndef PXA_ESP_INSTALLATIONS_INTERNAL_H
#define PXA_ESP_INSTALLATIONS_INTERNAL_H
#include "pxa_esp_installations.h"
#include "pxa_esp_package_trust.h"
#include "pxa/pxa_esp_storage.h"
/* Native transfer coordinator only. Normal consumers use the checked public
 * operations; this configuration is never exposed through a Guest ABI. */
struct pxa_esp_installations {
    pxa_posix_app_store_config_t config;
    pxa_esp_mbedtls_trust_t trust;
    const pxa_install_plan_t *plan;
    pxa_esp_app_storage_t *preparing_storage;
    struct pxa_esp_installations *media_owner;
    uint8_t resetting_data;
    pxa_volume_info_t volume;
    char root[PXA_ESP_STORAGE_PATH_BYTES+8];
    char resource_root[PXA_POSIX_APP_PATH_BYTES];
    char resource_package[PXA_POSIX_APP_PATH_BYTES];
    uint8_t resource_verified;
    uint8_t resource_loaded;
};
/* Receipt-owned archive transfer: private backend and local control records,
 * with no executable package or fabricated manifest. */
pxa_status_t pxa_esp_installations_plan_archive(pxa_esp_installations_t *,uint8_t,pxa_install_plan_t *);
pxa_status_t pxa_esp_installations_verify_media(void *,const pxa_install_record_t *);
#endif
