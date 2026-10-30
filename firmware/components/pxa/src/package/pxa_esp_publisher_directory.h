#ifndef PXA_ESP_PUBLISHER_DIRECTORY_H
#define PXA_ESP_PUBLISHER_DIRECTORY_H
#include "pxa/volume.h"
#include <stddef.h>
/* Host-only, volume-local display names. The authenticated installation locator
 * stays <full signer root>/<App ID>. No per-app NVS entries or Guest authority.
 * All mutations require the catalog transaction lock and an idle publisher. */
pxa_status_t pxa_esp_publisher_map(const char *pxa_root,const pxa_volume_id_t *,
    const char *kind,const char *locator,unsigned reverse,char *,size_t);
pxa_status_t pxa_esp_publisher_bind(const char *pxa_root,const pxa_volume_id_t *,
    const char *kind,const uint8_t publisher[32],const char *verified_handle);
/* Runtime reservation must not rename an existing shared media tree: other
 * applications can hold user-authorized directory handles below it. */
pxa_status_t pxa_esp_publisher_reserve(const char *pxa_root,const pxa_volume_id_t *,
    const char *kind,const uint8_t publisher[32],const char *verified_handle,uint8_t migrate_existing);
/* Physical path on a mounted volume; resolves the authenticated local name. */
pxa_status_t pxa_esp_publisher_path(uint64_t epoch,const char *kind,
    const char *logical_locator,char *,size_t);
#endif
