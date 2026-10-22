#ifndef PXA_ESP_APP_STORAGE_H
#define PXA_ESP_APP_STORAGE_H
#include "pxa/posix/pxa_posix_app_store.h"
#include "pxa/install_plan.h"
#include "pxa/permission.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pxa_esp_app_storage pxa_esp_app_storage_t;
/* Retain a verified resource mount continuously through activation/services.
 * One immutable binding per session; a conflicting rebind is rejected. */
pxa_status_t pxa_esp_app_storage_bind_media(pxa_esp_app_storage_t *,uint64_t mount_epoch);
/* Borrowed immutable binding; zero means this installation has no explicit
 * media binding. The storage owner retains the returned mount until close. */
uint64_t pxa_esp_app_storage_media_epoch(const pxa_esp_app_storage_t *);
/* Activation owner supplies a home already authenticated by app-store recover
 * (record MAC and signed code), the verified candidate of an approved
 * installation, or a record-authenticated maintenance home. Metadata-only
 * maintenance never authorizes code execution. One session per installation, shared
 * by its components. Pins the physical volume even before private FS use. */
pxa_status_t pxa_esp_app_storage_create(const pxa_posix_app_home_t *,uint64_t mount_epoch,
    uint64_t image_capacity_bytes,const pxa_install_plan_t *admitted_plan,pxa_esp_app_storage_t **);
/* Optional plan is borrowed only while preparing an installation. Runtime
 * sessions pass NULL; missing image creation then reserves its own capacity. */
/* Lazy private root. Direct backend when the volume promises private naming/
 * replace semantics, otherwise a LittleFS image on that same volume. Never
 * substitutes another volume. Returned path is Host-only, session-borrowed. */
pxa_status_t pxa_esp_app_storage_root(pxa_esp_app_storage_t *,const char **);
/* Migration source: require retained private state, never manufacture an
 * empty directory/image when the installation's storage is missing. */
pxa_status_t pxa_esp_app_storage_existing_root(pxa_esp_app_storage_t *,const char **);
/* Device-bound permission records live under the same private filesystem;
 * Guest FS hides .pxa-* entries. Missing permissions default to deny. */
pxa_status_t pxa_esp_app_storage_permissions(pxa_esp_app_storage_t *,pxa_permission_store_t *);
/* Host legacy conversion only. The source provider is a trusted read-only
 * snapshot of the old canonical identity. Fresh unpublished destination,
 * exact same publisher/App identity; seal current declared decisions with
 * the new installation's volume/InstallID/data binding. No source writes. */
pxa_status_t pxa_esp_app_storage_import_legacy_permissions(pxa_esp_app_storage_t *,
    pxa_bytes_t identity,const pxa_permission_store_t *source,
    const pxa_package_permission_t *,size_t count);
/* Migration only: idle source and unpublished destination, with both private
 * roots already opened. Same InstallID/AppID/publisher/data generation are
 * mandatory. Authenticate each declared source decision and reseal it for the
 * destination volume; raw .pxa-policy files are never transferable authority.
 * The caller retains its receipt/source until this and all data copying pass. */
pxa_status_t pxa_esp_app_storage_transfer_permissions(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,const pxa_package_permission_t *,size_t count);
/* Permission filenames are canonical SHA-256(name/scope) digests. A whole
 * move may preserve all authenticated decisions, including an archived
 * App's retained grants, without reconstructing old permission strings. */
pxa_status_t pxa_esp_app_storage_transfer_permission_tag(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,const uint8_t permission_digest[32],
    const uint8_t source_tag[32],uint8_t destination_tag[32]);
/* Authenticate an existing directory-grant body before rebinding its tag to
 * the same installation on another volume. The caller validates the format
 * and persists the body/new tag atomically in its unpublished destination. */
pxa_status_t pxa_esp_app_storage_transfer_grants(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,pxa_bytes_t body,const uint8_t source_tag[32],uint8_t destination_tag[32]);
pxa_status_t pxa_esp_app_storage_seal_grants(pxa_esp_app_storage_t *,pxa_bytes_t,uint8_t tag[32]);
pxa_status_t pxa_esp_app_storage_verify_grants(pxa_esp_app_storage_t *,pxa_bytes_t,const uint8_t tag[32]);
/* Close FS/KV users first. Failed unmount retains the owner for retry. */
pxa_status_t pxa_esp_app_storage_close(pxa_esp_app_storage_t **);
#ifdef __cplusplus
}
#endif
#endif
