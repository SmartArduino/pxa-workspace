#ifndef PXA_ESP_INSTALL_APPROVAL_H
#define PXA_ESP_INSTALL_APPROVAL_H
#include "pxa_esp_package_store.h"
#include "pxa/install_plan.h"
#include "pxa/install_record.h"
#include "pxa_esp_legacy_adoption.h"

/* Host-owned approval. No Guest paths or volume IDs enter Service 20. Keep
 * this object alive through selection, download and commit. After reservation
 * its plan is an owner lease and must never be copied. */
#define PXA_ESP_INSTALL_CHOICES 8u
typedef struct {
    pxa_volume_info_t volume;
    pxa_install_plan_t estimate;
    uint64_t temporary_bytes;
    uint8_t reuse;
} pxa_esp_install_choice_t;
typedef struct {
    pxa_install_record_t record;
    uint64_t expected_generation;
    uint8_t manifest_sha256[32];
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
    char locator[131];
    pxa_esp_install_choice_t choices[PXA_ESP_INSTALL_CHOICES];
    size_t count, selected;
    uint8_t download_only;
    uint8_t requires_resources;
    uint64_t resource_container_bytes;
    pxa_esp_install_choice_t media_choices[PXA_ESP_INSTALL_CHOICES];
    size_t media_count,media_selected;
    uint8_t media_fit[PXA_ESP_INSTALL_CHOICES];
    pxa_install_plan_t plan;
} pxa_esp_install_approval_t;

/* Native system move approval; source identity is immutable across the UI
 * round trip. Plans are estimates, never copied reservation owners. */
typedef struct {
    pxa_install_record_t source;
    uint64_t source_epoch;
    uint64_t source_media_epoch;
    uint8_t resources_only;
    uint8_t legacy_upgrade;
    pxa_esp_legacy_source_t legacy;
    char identity[PXA_ESP_PACKAGE_ID_BYTES],locator[131];
    char name[64],version[32],current_volume[64],media_volume[64];
    pxa_esp_install_choice_t choices[PXA_ESP_INSTALL_CHOICES];
    size_t count;
} pxa_esp_move_approval_t;
pxa_status_t pxa_esp_package_store_prepare_move(const char *,pxa_esp_move_approval_t *);
pxa_status_t pxa_esp_package_store_prepare_move_resources(const char *,pxa_esp_move_approval_t *);
pxa_status_t pxa_esp_package_store_commit_move(const pxa_esp_move_approval_t *,size_t,
    pxa_esp_transfer_progress_fn,void *);

/* Authenticates only the bounded signed prefix, snapshots the current local
 * record and evaluates actual geometry including transfer peak. No writes. */
pxa_status_t pxa_esp_package_store_prepare_install(const uint8_t *,size_t,uint64_t,uint64_t transfer_bytes,uint8_t download_only,
    const uint8_t expected_root[32],const char *expected_app_id,
    pxa_esp_package_preview_t *,pxa_esp_install_approval_t *);
/* Selected choice belongs to the above immutable snapshot. Refreshes capacity
 * before all-or-nothing reservation. No fallback to a different volume. */
pxa_status_t pxa_esp_package_store_reserve_install(pxa_esp_install_approval_t *,size_t);
/* A NULL prefix permits only already prepared generations (offline code
 * cache); otherwise authenticate the exact resource manifest bound by code.
 * Estimates are independent of selection; only compatible pairs may reserve. */
pxa_status_t pxa_esp_package_store_prepare_resources(pxa_esp_install_approval_t *,
    const uint8_t *prefix,size_t,uint64_t container_bytes,uint64_t transfer_bytes);
pxa_status_t pxa_esp_package_store_evaluate_install(const pxa_esp_install_approval_t *,
    size_t app_choice,size_t media_choice,pxa_install_plan_t *);
/* Same approval/lease as code commit; source contains the resource package.
 * A prepared immutable generation remains reusable if code commit is retried. */
pxa_status_t pxa_esp_package_store_commit_resources(const char *,pxa_esp_install_approval_t *);
/* Exact approved manifest + record generation CAS. The source is fully
 * authenticated by the installer even when already-current. */
pxa_status_t pxa_esp_package_store_commit_install(const char *,pxa_esp_install_approval_t *);

/* Native Files owns one noncopyable source lease through confirmation and
 * commit. Initialize to zero and set source before prepare. close is safe on
 * every error/cancel path; it never removes the user's input file. */
typedef struct {
    pxa_esp_install_approval_t placement;
    pxa_esp_package_preview_t preview;
    char source[512];
    uint64_t source_epoch;
    uint8_t resources_only;
} pxa_esp_file_install_t;
pxa_status_t pxa_esp_file_install_prepare(pxa_esp_file_install_t *);
pxa_status_t pxa_esp_file_install_commit(pxa_esp_file_install_t *,size_t app,size_t media);
void pxa_esp_file_install_close(pxa_esp_file_install_t *);
#endif
