#ifndef PXA_ESP_APP_TRANSFER_H
#define PXA_ESP_APP_TRANSFER_H
#include "pxa_esp_installations.h"
typedef struct {
    void *context;
    /* Cancellation/cooperative yield before surrender. Once the source has
     * surrendered, recovery finishes the handoff instead of undoing it. */
    pxa_status_t (*check)(void *);
    /* 0: cooperative poll; 1: both COPYING receipts are durable. This
     * callback borrows the operation thread; no catalog/volume re-entry. */
    void (*progress)(void *,uint8_t);
} pxa_esp_app_transfer_control_t;
/* Trusted Host only, under the catalog transaction lock and with both homes
 * idle. Moves signed code, private state and authority together. Immutable
 * resources keep their authenticated volume binding. No primary registry.
 * An error may leave an authenticated pending transfer; retry resume(), never
 * erase its target or reactivate a retired source on an uncertain outcome. */
/* Archived homes move private state and sealed records without code. Their
 * archive flag is preserved and result is unused; reinstallation requires a
 * separately verified signed package.
 * result may be NULL: the coordinator owns bounded PSRAM buffers measured
 * from the fixed signed manifest paths, including a retired source package. */
pxa_status_t pxa_esp_app_transfer_begin(uint64_t source_epoch,uint64_t target_epoch,
    const char *locator,uint64_t expected_generation,const pxa_esp_app_transfer_control_t *,
    pxa_posix_installer_result_t *,pxa_posix_app_home_t *);
/* Read-only placement preview, without reservation or namespace creation.
 * Caller serializes the source home, then keeps this complete source record
 * through confirmation. The approved begin rejects replacement/reinstall or
 * generation changes, even when a new installation reused generation one. */
pxa_status_t pxa_esp_app_transfer_plan(uint64_t source_epoch,uint64_t target_epoch,
    const char *locator,uint64_t expected_generation,pxa_install_record_t *,pxa_install_plan_t *);
pxa_status_t pxa_esp_app_transfer_begin_approved(uint64_t source_epoch,uint64_t target_epoch,
    const char *locator,const pxa_install_record_t *,const pxa_esp_app_transfer_control_t *,
    pxa_posix_installer_result_t *,pxa_posix_app_home_t *);
/* Resume from a target-local receipt, including after a reboot/remount. A
 * durable surrender proof permits finishing prepared target state even when
 * the source is offline. A missing source before proof stays pending. */
pxa_status_t pxa_esp_app_transfer_resume(uint64_t target_epoch,const char *locator,
    const pxa_esp_app_transfer_control_t *,pxa_posix_installer_result_t *,pxa_posix_app_home_t *);
/* Metadata-only target discovery. Presence is quarantine, not authority;
 * resume authenticates every receipt. Dot-prefixed source intent roots are
 * excluded. Visitor returns zero to stop; no tree-sized allocation. */
typedef int (*pxa_esp_app_transfer_visitor_fn)(void *,const char *locator);
pxa_status_t pxa_esp_app_transfer_visit(uint64_t epoch,pxa_esp_app_transfer_visitor_fn,void *,size_t *count);
#endif
