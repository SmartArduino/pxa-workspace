#ifndef PXA_ESP_LEGACY_UPDATE_H
#define PXA_ESP_LEGACY_UPDATE_H
#include "pxa/package.h"
#include <string.h>

/* Both manifests must already be authenticated. Built-ins have no writable
 * current slot for the installer to compare, so validate their overlay here.
 * An update may extend a signing lineage, never replace or truncate it. */
static inline pxa_status_t pxa_esp_legacy_update_allowed(
    const pxa_package_manifest_t *current,const pxa_package_manifest_t *incoming) {
    pxa_package_publisher_lineage_t old,next;
    if(!current || !incoming || current->app_id.size!=incoming->app_id.size ||
        memcmp(current->app_id.data,incoming->app_id.data,current->app_id.size) ||
        (current->has_release_sequence && (!incoming->has_release_sequence ||
         incoming->release_sequence<current->release_sequence)))return PXA_STATUS_DENIED;
    if(pxa_package_publisher_lineage_parse(current,&old)!=PXA_STATUS_OK ||
        pxa_package_publisher_lineage_parse(incoming,&next)!=PXA_STATUS_OK ||
        memcmp(old.root_key_id,next.root_key_id,32) || next.link_count<old.link_count)return PXA_STATUS_DENIED;
    for(size_t i=0;i<old.link_count;++i) {
        const pxa_package_lineage_link_t *a=&old.links[i],*b=&next.links[i];
        if(a->generation!=b->generation || a->flags!=b->flags ||
            memcmp(a->old_key_id,b->old_key_id,32) || memcmp(a->new_key_id,b->new_key_id,32) ||
            a->new_spki.size!=b->new_spki.size || memcmp(a->new_spki.data,b->new_spki.data,a->new_spki.size) ||
            memcmp(a->signature,b->signature,64))return PXA_STATUS_DENIED;
    }
    if(current->has_release_sequence && incoming->release_sequence==current->release_sequence &&
        (current->encoded.size!=incoming->encoded.size ||
         memcmp(current->encoded.data,incoming->encoded.data,current->encoded.size)))return PXA_STATUS_DENIED;
    return PXA_STATUS_OK;
}
#endif
