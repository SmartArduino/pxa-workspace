#ifndef PXA_ESP_PRIVATE_AUTHORITY_H
#define PXA_ESP_PRIVATE_AUTHORITY_H
#include "pxa_esp_app_storage.h"
/* Idle source and freshly copied unpublished destination. Uses bounded PSRAM
 * scratch. Retains every authenticated permission and directory grant while
 * binding its authority to the destination volume. No raw tags are trusted. */
pxa_status_t pxa_esp_private_authority_copy(pxa_esp_app_storage_t *,pxa_esp_app_storage_t *);
/* Legacy converter owns/leases a protected old grants directory (or -1 when
 * it never existed). grant_name is the exact canonical source identity.
 * Verify the old local checksum/format before resealing directory grants;
 * import only signed-manifest permission declarations through a trusted
 * read-only provider. Destination control entries must be absent. Caller
 * retains its durable conversion intent through any partial failure. */
pxa_status_t pxa_esp_private_authority_import_legacy(pxa_esp_app_storage_t *,
    pxa_bytes_t identity,const pxa_permission_store_t *source,
    const pxa_package_permission_t *,size_t count,int grants_directory,const char *grant_name);
/* Read-only preflight before retiring executable code. Same checksum/format
 * validation as import, plus each declared permission decision. Never creates
 * private state or lazily migrates the legacy provider. */
pxa_status_t pxa_esp_private_authority_validate_legacy(pxa_bytes_t identity,
    const pxa_permission_store_t *,const pxa_package_permission_t *,size_t,
    int grants_directory,const char *grant_name);
#endif
