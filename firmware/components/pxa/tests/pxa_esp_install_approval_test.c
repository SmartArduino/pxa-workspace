#include "pxa_esp_install_approval.h"
#include "pxa/pxa_esp_storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pxa_volume_registry_t *registry;
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *info) {
    return pxa_volume_get(registry,epoch,info);
}
pxa_status_t pxa_esp_storage_plan(const pxa_install_requirements_t *required,
    uint64_t app,uint64_t media,pxa_install_plan_t *plan) {
    return pxa_install_plan_evaluate(registry,required,app,media,plan);
}
static void estimate(pxa_esp_install_choice_t *choice,uint64_t epoch,uint64_t bytes) {
    pxa_install_requirements_t required={.app_bytes=bytes,.volume_headroom_bytes=4096};
    assert(!pxa_volume_get(registry,epoch,&choice->volume));
    assert(!pxa_install_plan_evaluate(registry,&required,epoch,0,&choice->estimate));
}
int main(void) {
    void *memory=calloc(1,pxa_volume_registry_workspace_size(2));assert(memory);
    assert(!pxa_volume_registry_init(memory,pxa_volume_registry_workspace_size(2),2,100,&registry));
    pxa_volume_info_t volume={0};uint64_t first,second;
    volume.id.bytes[0]=1;volume.device_id.bytes[0]=1;volume.state=PXA_VOLUME_READY;
    volume.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ|PXA_VOLUME_INSTALLABLE;
    volume.total_bytes=10000;volume.free_bytes=10000;volume.space_known=3;
    assert(!pxa_volume_register(registry,&volume,&first));
    volume.id.bytes[0]=2;assert(!pxa_volume_register(registry,&volume,&second));
    pxa_esp_install_approval_t approval={0};pxa_install_plan_t plan;
    approval.count=1;approval.requires_resources=1;approval.media_count=1;
    estimate(&approval.choices[0],first,700);estimate(&approval.media_choices[0],first,500);
    assert(!pxa_volume_update(registry,first,PXA_VOLUME_READY,10000,5296,3));
    assert(!pxa_esp_package_store_evaluate_install(&approval,0,0,&plan));
    assert(plan.volume_count==1 && plan.volumes[0].bytes==5296); /* One shared headroom. */
    approval.choices[0].temporary_bytes=200;approval.media_choices[0].temporary_bytes=300;
    assert(!pxa_volume_update(registry,first,PXA_VOLUME_READY,10000,4996,3));
    assert(!pxa_esp_package_store_evaluate_install(&approval,0,0,&plan));
    assert(plan.volumes[0].bytes==4996); /* Resource transport and new private state do not coexist. */
    approval.choices[0].temporary_bytes=0;approval.media_choices[0].temporary_bytes=0;
    assert(!pxa_volume_update(registry,first,PXA_VOLUME_READY,10000,5295,3));
    assert(pxa_esp_package_store_evaluate_install(&approval,0,0,&plan)==PXA_STATUS_QUOTA_EXCEEDED);
    assert(!plan.volume_count && !plan.reserved);
    assert(!pxa_volume_update(registry,first,PXA_VOLUME_READY,10000,4796,3));
    estimate(&approval.media_choices[0],second,500);
    assert(!pxa_volume_update(registry,second,PXA_VOLUME_READY,10000,4596,3));
    assert(!pxa_esp_package_store_evaluate_install(&approval,0,0,&plan));
    assert(plan.volume_count==2 && plan.volumes[0].bytes==4796 && plan.volumes[1].bytes==4596);
    assert(!pxa_volume_update(registry,second,PXA_VOLUME_READY,10000,4595,3));
    assert(pxa_install_plan_reserve(registry,&plan)==PXA_STATUS_QUOTA_EXCEEDED);
    pxa_volume_info_t state;assert(!pxa_volume_get(registry,first,&state) && !state.reserved_bytes);
    assert(!pxa_volume_update(registry,second,PXA_VOLUME_READY,10000,4596,3));
    assert(!pxa_esp_package_store_evaluate_install(&approval,0,0,&approval.plan));
    assert(!pxa_install_plan_reserve(registry,&approval.plan));
    assert(pxa_esp_package_store_evaluate_install(&approval,0,0,&approval.plan)==PXA_STATUS_BAD_STATE);
    assert(approval.plan.reserved && approval.plan.volume_count==2); /* Owner lease not cleared. */
    assert(!pxa_install_plan_release(registry,&approval.plan));
    assert(!pxa_volume_unregister(registry,second));
    volume.capabilities=PXA_VOLUME_READ|PXA_VOLUME_RANDOM_READ;volume.free_bytes=0;
    assert(!pxa_volume_register(registry,&volume,&second));
    memset(&approval.media_choices[0],0,sizeof(approval.media_choices[0]));
    assert(!pxa_volume_get(registry,second,&approval.media_choices[0].volume));approval.media_choices[0].reuse=1;
    assert(!pxa_esp_package_store_evaluate_install(&approval,0,0,&plan));
    assert(plan.volume_count==1 && plan.volumes[0].bytes==4796); /* Read-only prepared media consumes no disk. */
    assert(pxa_esp_package_store_evaluate_install(&approval,0,1,&plan)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_volume_unregister(registry,second));
    assert(pxa_esp_package_store_evaluate_install(&approval,0,0,&plan)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_volume_unregister(registry,first));
    free(memory);puts("Native placement: shared headroom, two-volume admission rollback, immutable reuse and lease ownership passed");
    return 0;
}
