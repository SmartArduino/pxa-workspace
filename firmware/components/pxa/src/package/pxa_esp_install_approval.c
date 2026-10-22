#include "pxa_esp_install_approval.h"
#include "pxa/pxa_esp_storage.h"
#include <string.h>

pxa_status_t pxa_esp_package_store_evaluate_install(const pxa_esp_install_approval_t *approval,
    size_t app,size_t media,pxa_install_plan_t *plan) {
    if(!approval || !plan || approval->count>PXA_ESP_INSTALL_CHOICES ||
        approval->media_count>PXA_ESP_INSTALL_CHOICES || app>=approval->count)return PXA_STATUS_INVALID_ARGUMENT;
    if(approval->plan.reserved)return PXA_STATUS_BAD_STATE;
    memset(plan,0,sizeof(*plan));
    if(!approval->requires_resources) {
        if(approval->choices[app].estimate.reserved)return PXA_STATUS_BAD_STATE;
        *plan=approval->choices[app].estimate;return PXA_STATUS_OK;
    }
    if(media>=approval->media_count)return PXA_STATUS_UNAVAILABLE;
    const pxa_esp_install_choice_t *application=&approval->choices[app],*resources=&approval->media_choices[media];
    if(application->estimate.reserved || resources->estimate.reserved || application->estimate.volume_count!=1 || application->estimate.volumes[0].bytes<=4096 ||
        (!resources->reuse && (resources->estimate.volume_count!=1 || resources->estimate.volumes[0].bytes<=4096)))
        return PXA_STATUS_BAD_STATE;
    if(resources->reuse) {
        pxa_volume_info_t mounted;
        pxa_status_t status=pxa_esp_storage_volume(resources->volume.mount_epoch,&mounted);
        if(status!=PXA_STATUS_OK || mounted.state!=PXA_VOLUME_READY || mounted.ambiguous ||
            !(mounted.capabilities&PXA_VOLUME_READ) || memcmp(mounted.id.bytes,resources->volume.id.bytes,16))
            return PXA_STATUS_UNAVAILABLE;
    }
    /* Per-volume estimates include one headroom allowance. Combining them
     * must charge that allowance once when both destinations are identical. */
    pxa_install_requirements_t required={.app_bytes=application->estimate.volumes[0].bytes-4096,
        .media_bytes=resources->reuse?0:resources->estimate.volumes[0].bytes-4096,.volume_headroom_bytes=4096};
    if(application->temporary_bytes>required.app_bytes || resources->temporary_bytes>required.media_bytes)
        return PXA_STATUS_BAD_STATE;
    if(application->volume.mount_epoch==resources->volume.mount_epoch && !resources->reuse) {
        /* Resource transport is retired before preparing code/private state.
         * These two phases share a volume, so reserve their maximum rather
         * than charging both short-lived copies at the same time. */
        uint64_t app_new=required.app_bytes-application->temporary_bytes;
        uint64_t media_new=required.media_bytes-resources->temporary_bytes;
        uint64_t phase=app_new>resources->temporary_bytes?app_new:resources->temporary_bytes;
        if(media_new>UINT64_MAX-application->temporary_bytes ||
            phase>UINT64_MAX-media_new-application->temporary_bytes)return PXA_STATUS_LIMIT_EXCEEDED;
        required.app_bytes=application->temporary_bytes+media_new+phase;required.media_bytes=0;
    }
    return pxa_esp_storage_plan(&required,application->volume.mount_epoch,resources->volume.mount_epoch,plan);
}
