#include "pxa_esp_install_capacity.h"
#include "pxa/pxa_esp_storage.h"
/* System-owned Files jobs. Included after native move state so both flows
 * share the existing runtime owner, prompt sequence and modal lifetime. */
struct pxa_native_file {
    pxa_esp_file_install_t file;
    uint32_t prompt_id;
};
static void native_file_dispose(pxa_native_file_t *job) {
    if(!job)return;
    pxa_esp_file_install_close(&job->file);free(job);
}
static void native_file_cancel(void) {
    pxa_native_file_t *job=g_native_file;if(!job)return;
    g_native_file=NULL;pxa_esp_ui_shell_dismiss_install_prompt(job->prompt_id);
    pxa_esp_surface_runtime_modal_leave();native_file_dispose(job);
}
static void install_prompt_fill(pxa_esp_ui_install_prompt_t *prompt,const pxa_esp_package_preview_t *preview,
    const pxa_esp_install_approval_t *approval,uint8_t resources_only) {
    copy_utf8_c_string(prompt->app_name,sizeof(prompt->app_name),preview->name);
    copy_utf8_c_string(prompt->version,sizeof(prompt->version),preview->version);
    snprintf(prompt->publisher,sizeof(prompt->publisher),"%.16s",preview->id);
    prompt->count=(uint8_t)approval->count;prompt->selected=(uint8_t)approval->selected;
    prompt->permission_count=preview->permission_count;prompt->resources_only=resources_only;
    prompt->media_count=resources_only?0:(uint8_t)approval->media_count;
    prompt->media_selected=(uint8_t)approval->media_selected;
    memset(prompt->media_fit,0,sizeof(prompt->media_fit));
    memset(prompt->same_media,UINT8_MAX,sizeof(prompt->same_media));
    for(size_t i=0;i<approval->count;++i) {
        const pxa_esp_install_choice_t *choice=&approval->choices[i];
        copy_utf8_c_string(prompt->volumes[i].name,sizeof(prompt->volumes[i].name),choice->volume.name);
        prompt->volumes[i].required_bytes=choice->estimate.volumes[0].bytes;
        for(size_t j=0;j<prompt->media_count;++j) {
            if(choice->volume.mount_epoch==approval->media_choices[j].volume.mount_epoch)prompt->same_media[i]=(uint8_t)j;
            pxa_install_plan_t estimate;
            if(pxa_esp_package_store_evaluate_install(approval,i,j,&estimate)==PXA_STATUS_OK) {
                prompt->combined_bytes[i][j]=estimate.volumes[0].bytes;
                prompt->media_fit[i]|=(uint8_t)(1u<<j);
            }
        }
    }
    for(size_t i=0;i<prompt->media_count;++i) {
        const pxa_esp_install_choice_t *choice=&approval->media_choices[i];
        copy_utf8_c_string(prompt->media_volumes[i].name,sizeof(prompt->media_volumes[i].name),choice->volume.name);
        pxa_volume_info_t current;
        pxa_esp_install_volume_capacity(&prompt->media_volumes[i],
            pxa_esp_storage_volume(choice->volume.mount_epoch,&current)==PXA_STATUS_OK?&current:NULL);
        prompt->media_volumes[i].required_bytes=choice->reuse?0:choice->estimate.volumes[0].bytes;
        prompt->media_reuse[i]=choice->reuse;
    }
    /* Pair evaluation refreshes filesystem capacity. Read the final snapshot,
     * rather than the older per-choice value from before resource preflight. */
    for(size_t i=0;i<approval->count;++i) {
        pxa_volume_info_t current;
        pxa_esp_install_volume_capacity(&prompt->volumes[i],
            pxa_esp_storage_volume(approval->choices[i].volume.mount_epoch,&current)==PXA_STATUS_OK?&current:NULL);
    }
}
static __attribute__((noinline)) void native_file_prepare(pxa_native_file_t *job) {
    if(!job)return;
    if(g_host.cooperative_active) {native_file_dispose(job);return;}
    const bool zh=pxa_esp_host_locale_is_chinese();
    if(g_native_file || g_native_move || g_store_install || g_store_result.prompt_id || g_store_uninstall.prompt_id ||
        g_host.activation.active_identity[0] || g_host.graceful_stop_active || g_host.shutdown_complete ||
        g_host.deferred_launch_identity[0] || pxa_esp_ime_active()) {
        native_file_dispose(job);pxa_esp_ui_shell_post_toast(zh?"请先关闭正在运行的应用":"Close the running app first",2500);return;
    }
    job->prompt_id=++g_host.next_permission_prompt_id;
    if(!job->prompt_id)job->prompt_id=++g_host.next_permission_prompt_id;
    g_native_file=job;pxa_esp_surface_runtime_modal_enter();
    const char *name=strrchr(job->file.source,'/');
    if(!pxa_esp_ui_shell_post_file_prepare_progress(job->prompt_id,name?name+1:job->file.source)) {
        native_file_cancel();return;
    }
    pxa_status_t status=pxa_esp_file_install_prepare(&job->file);
    if(status!=PXA_STATUS_OK) {
        const bool missing=job->file.placement.requires_resources && !job->file.resources_only && !job->file.placement.media_count;
        native_file_cancel();
        pxa_esp_ui_shell_post_toast(missing?(zh?"请先安装此应用所需的资源包":"Install this app's resource package first"):
            status==PXA_STATUS_QUOTA_EXCEEDED?(zh?"没有足够空间的安装位置":"No storage has enough free space"):
            status==PXA_STATUS_UNSUPPORTED?(zh?"此安装包暂不支持本地安装":"This package cannot be installed here yet"):
            (zh?"无法校验安装包，请检查文件与存储设备":"Cannot verify package; check file and storage"),3000);return;
    }
    pxa_esp_ui_install_prompt_t *prompt=heap_caps_calloc(1,sizeof(*prompt),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!prompt) {native_file_cancel();pxa_esp_ui_shell_post_toast(zh?"内存不足":"Not enough memory",2500);return;}
    prompt->prompt_id=job->prompt_id;install_prompt_fill(prompt,&job->file.preview,&job->file.placement,job->file.resources_only);
    if(!pxa_esp_ui_shell_post_install_prompt(prompt))native_file_cancel();
    free(prompt);
}
static void native_file_decide(bool approved,uint8_t encoded) {
    pxa_native_file_t *job=g_native_file;if(!job)return;
    if(!approved || g_host.activation.active_identity[0] || g_host.graceful_stop_active || pxa_esp_ime_active()) {
        native_file_cancel();return;
    }
    pxa_esp_install_approval_t *placement=&job->file.placement;
    size_t app=encoded?((encoded-1u)&7u):placement->selected;
    size_t media=encoded?((encoded-1u)>>3):(job->file.resources_only?0:placement->media_selected);
    pxa_status_t status=PXA_STATUS_RESOURCE_LIMIT;
    if(pxa_esp_ui_shell_post_file_install_progress(job->prompt_id,job->file.preview.name,job->file.resources_only))
        status=pxa_esp_file_install_commit(&job->file,app,media);
    pxa_esp_surface_runtime_modal_leave();g_native_file=NULL;
    pxa_esp_ui_shell_post_file_install_result(job->prompt_id,status);
    native_file_dispose(job);pxa_host_notify_catalog_changed();
}
