#ifndef PXA_ESP_LEGACY_ADOPTION_H
#define PXA_ESP_LEGACY_ADOPTION_H
#include "pxa_esp_app_transfer.h"

#define PXA_ESP_LEGACY_PATH_BYTES 160u
/* Trusted catalog snapshot, never Guest-supplied paths. Paths are relative to
 * their physical mount. Code/private must belong to the old native layout;
 * manual media stays on its actual volume and is renamed, never recopied. */
typedef struct {
    uint64_t app_epoch,media_epoch;
    char package[PXA_ESP_LEGACY_PATH_BYTES];
    char private_root[PXA_ESP_LEGACY_PATH_BYTES];
    char media[PXA_ESP_LEGACY_PATH_BYTES];
    uint8_t private_present;
} pxa_esp_legacy_source_t;
/* Read-only signed-code verification, conflict checks and metadata admission.
 * Captures whether private state existed and creates a new InstallID in the
 * returned approved record. No directory, permission or registry writes. */
pxa_status_t pxa_esp_legacy_adoption_prepare(pxa_esp_legacy_source_t *,uint8_t enabled,
    pxa_install_record_t *,pxa_install_plan_t *);
/* Idle legacy source only. Persist an authenticated intent before retiring
 * old code; move private state only after retirement. Partial errors retain
 * the journal and the only copy of the data for resume, never roll back by
 * inventing an empty old root. home is populated only after publication. */
pxa_status_t pxa_esp_legacy_adoption_begin(const pxa_esp_legacy_source_t *,
    const pxa_install_record_t *,const pxa_esp_app_transfer_control_t *,pxa_posix_app_home_t *);
/* Authenticates the journal before interpreting paths. UNSUPPORTED denotes
 * another journal kind, so the owner can dispatch to its normal resumer. */
pxa_status_t pxa_esp_legacy_adoption_resume(uint64_t,const char *,
    const pxa_esp_app_transfer_control_t *,pxa_posix_app_home_t *);
#endif
