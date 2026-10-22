#ifndef PXA_ESP_RESOURCE_TRANSFER_H
#define PXA_ESP_RESOURCE_TRANSFER_H
#include "pxa_esp_installations.h"
/* Host catalog transaction + idle app lease required. Preview writes nothing.
 * source_media_epoch pins the physical mount observed during confirmation;
 * commit rejects replacement/reinstall/remount, even for the same UUID.
 * Copy immutable signed resources, then CAS/mirror only the app-local record.
 * Source resources remain recoverable until a separate ownership-aware GC. */
pxa_status_t pxa_esp_resource_transfer_plan(uint64_t app_epoch,uint64_t target_epoch,
    const char *locator,uint64_t expected_generation,pxa_install_record_t *,
    uint64_t *source_media_epoch,pxa_install_plan_t *);
pxa_status_t pxa_esp_resource_transfer_begin_approved(uint64_t app_epoch,uint64_t source_media_epoch,
    uint64_t target_epoch,const char *locator,const pxa_install_record_t *,pxa_posix_app_home_t *);
#endif
