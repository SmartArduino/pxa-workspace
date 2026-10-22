#ifndef PXA_ESP_INSTALLATIONS_H
#define PXA_ESP_INSTALLATIONS_H
#include "pxa/posix/pxa_posix_app_store.h"
#include "pxa/install_plan.h"
#include "pxa_esp_app_storage.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pxa_esp_installations pxa_esp_installations_t;
/* One owner per operation, allocated in PSRAM. No primary filesystem or
 * per-app NVS registry. Pins this precise mount until close. Owner serializes
 * calls and keeps result buffers/source immutable through approval/commit.
 * The Host also serializes code mutation/activation per home across owners. */
pxa_status_t pxa_esp_installations_open(uint64_t epoch,pxa_esp_installations_t **);
/* A failed private unmount keeps the owner intact for retry. */
pxa_status_t pxa_esp_installations_close(pxa_esp_installations_t **);
pxa_status_t pxa_esp_installations_visit(pxa_esp_installations_t *,
    pxa_posix_app_home_visitor_fn,void *,size_t *invalid);
/* Signed identity inspection only; payload is verified again during install. */
pxa_status_t pxa_esp_installations_inspect(pxa_esp_installations_t *,const char *,
    pxa_posix_installer_result_t *);
/* Immutable, signed Manifest 0.8 resource generations on record.media_volume.
 * Install checks the reservation before writing and binds the exact digest,
 * publisher and App ID during every verification/recovery phase. No app home,
 * private image, central registry or executable component is created here. */
pxa_status_t pxa_esp_installations_install_resources(pxa_esp_installations_t *,
    const pxa_install_record_t *,const char *source,const pxa_install_plan_t *,
    pxa_posix_installer_result_t *);
pxa_status_t pxa_esp_installations_verify_resources(pxa_esp_installations_t *,
    const pxa_install_record_t *,pxa_posix_installer_result_t *);
pxa_status_t pxa_esp_installations_media_path(const pxa_install_record_t *,char *,size_t);
/* HAS_MEDIA without MANAGED_MEDIA binds mutable, user-imported media to the
 * canonical publisher/App directory on one precise volume. Its digest must
 * be zero. Activation retains that mount and requires the directory to exist;
 * this does not turn manual assets into signed resources. */
/* Fixed namespace derived from the signed publisher root, never a Guest path.
 * Human publisher aliases can be added by a separately authenticated policy. */
pxa_status_t pxa_esp_installations_locator(const uint8_t root[32],const char *app_id,char *,size_t);
/* Budget the selected volume's allocation geometry, decoded files, directories,
 * durable control records, private image and caller's actual transfer peak.
 * This evaluates only; the Host confirms placement before reserve_plan(). */
pxa_status_t pxa_esp_installations_plan(pxa_esp_installations_t *,
    const pxa_package_manifest_t *,uint64_t transfer_bytes,uint8_t new_private,
    pxa_install_plan_t *);
/* approved binds the intended identity, install ID, previous generation and
 * optional already-prepared media generation. The exact approved signed
 * manifest digest prevents changing permissions between preview and install.
 * Required media must have a Host verifier; this API never accepts presence
 * of a volume as proof that its resource pack is authentic/complete. */
pxa_status_t pxa_esp_installations_install(pxa_esp_installations_t *,const char *,
    const char *locator,const pxa_install_record_t *approved,uint64_t expected_generation,
    const uint8_t approved_manifest_sha256[32],const pxa_install_plan_t *,
    pxa_posix_media_ready_fn,void *,pxa_posix_installer_result_t *,pxa_posix_app_home_t *);
pxa_status_t pxa_esp_installations_load(pxa_esp_installations_t *,const char *locator,
    pxa_posix_media_ready_fn,void *,pxa_posix_installer_result_t *,pxa_posix_app_home_t *);
pxa_status_t pxa_esp_installations_lookup(pxa_esp_installations_t *,const char *locator,pxa_posix_app_home_t *);
/* Maintenance provider borrows this operation's private session. Only use a
 * home returned by lookup/load on this owner. Close the owner after use. */
pxa_status_t pxa_esp_installations_permissions(pxa_esp_installations_t *,const pxa_posix_app_home_t *,pxa_permission_store_t *);
pxa_status_t pxa_esp_installations_set_enabled(pxa_esp_installations_t *,const char *locator,
    uint64_t expected_generation,uint8_t enabled,pxa_posix_app_home_t *);
/* Idle home only. Prepares a fresh private generation, commits both local
 * record slots, then retires the previous data root. Output remains populated
 * if only post-commit garbage collection failed. */
pxa_status_t pxa_esp_installations_reset_data(pxa_esp_installations_t *,const char *locator,
    uint64_t expected_generation,uint64_t expected_data_generation,pxa_posix_app_home_t *);
/* Requires the Host's idle-home lease. Archive retains private data and local
 * identity; full removal retires the entire home after the archive commits. */
pxa_status_t pxa_esp_installations_remove(pxa_esp_installations_t *,const char *locator,
    uint64_t expected_generation,uint8_t keep_data,pxa_posix_app_home_t *);
/* Caller owns the returned activation session and closes it after all services.
 * Home must be the verified result of load/install, not a discovery snapshot. */
pxa_status_t pxa_esp_installations_storage(pxa_esp_installations_t *,
    const pxa_posix_app_home_t *,pxa_esp_app_storage_t **);
#ifdef __cplusplus
}
#endif
#endif
