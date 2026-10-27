/* SPDX-License-Identifier: MIT */
#include "pxa_esp_ui_shell.h"
#include "pxa_esp_system_overlay.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa_esp_host.h"
#include "pxa_esp_dialog_style.h"
#include "pxa/lvgl/pxa_lvgl_file_picker.h"
#include <stdlib.h>
#include <string.h>

/* Reuse the same themed picker and navigation as file imports. A paused
 * timer owns completion so an LVGL click never destroys its callback state. */
typedef struct {
    pxa_esp_ui_directory_prompt_t prompt;
    pxa_lvgl_file_picker_t *picker;
    lv_timer_t *completion;
} directory_picker_t;
static directory_picker_t *g_picker;
/* Updated under the LVGL lock before enqueueing: cancellation or a newer
 * request invalidates older show callbacks regardless of timer order. */
static uint32_t g_requested;
static uint32_t g_pending_back;
static void close_picker(void) {
    directory_picker_t *p=g_picker;if(!p)return;
    g_picker=NULL;lv_timer_delete(p->completion);
    pxa_lvgl_file_picker_destroy(p->picker);free(p);
}
static void overlay(lv_obj_t *object,int action) {
    if(action>0)pxa_esp_system_overlay_add(object);
    else if(action<0)pxa_esp_system_overlay_remove(object);
    else pxa_esp_system_overlay_refresh();
}
static void changed(void *context) {
    directory_picker_t *p=context;lv_timer_resume(p->completion);lv_timer_ready(p->completion);
}
static void complete(lv_timer_t *timer) {
    directory_picker_t *p=lv_timer_get_user_data(timer);
    if(p!=g_picker)return;
    unsigned state=pxa_lvgl_file_picker_state(p->picker);
    if(!state) {lv_timer_pause(timer);return;}
    size_t count=0;const pxa_file_choice_t *choice=pxa_lvgl_file_picker_selection(p->picker,&count);
    (void)pxa_esp_storage_choose_directory(p->prompt.prompt_id,
        state==1 && count==1?choice[0].epoch:0,state==1 && count==1?choice[0].path:"");
    if(g_requested==p->prompt.prompt_id)g_requested=0;
    close_picker();
}
static pxa_status_t emit(void *context,size_t index,uint8_t kind,pxa_bytes_t name) {
    pxa_file_picker_entry_t *rows=context;
    if(kind!=PXA_FS_KIND_DIRECTORY || name.size>PXA_FS_SCOPED_MAX_PATH_BYTES)return PXA_STATUS_PROTOCOL_ERROR;
    char *copy=malloc(name.size+1);if(!copy)return PXA_STATUS_RESOURCE_LIMIT;
    memcpy(copy,name.data,name.size);copy[name.size]=0;
    rows[index]=(pxa_file_picker_entry_t){0,kind,copy};return PXA_STATUS_OK;
}
static pxa_status_t browse(void *context,uint64_t epoch,const char *path,const char *filter,
    size_t skip,pxa_file_picker_entry_t *rows,size_t capacity,size_t *count,uint8_t *more) {
    directory_picker_t *p=context;(void)filter;
    if(epoch)return pxa_esp_storage_directory_entries(p->prompt.prompt_id,epoch,path,
        skip,capacity,count,more,emit,rows);
    pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];size_t seen=0;
    size_t n=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);*count=0;*more=0;
    for(size_t i=0;i<n;++i) {
        if(volumes[i].state!=PXA_VOLUME_READY || volumes[i].ambiguous || !(volumes[i].capabilities&PXA_VOLUME_READ))continue;
        if(seen++<skip)continue;
        if(*count==capacity) {*more=1;break;}
        size_t size=strlen(volumes[i].name);char *name=malloc(size+1);if(!name)return PXA_STATUS_RESOURCE_LIMIT;
        memcpy(name,volumes[i].name,size+1);
        rows[(*count)++]=(pxa_file_picker_entry_t){volumes[i].mount_epoch,PXA_FS_KIND_DIRECTORY,name};
    }
    return PXA_STATUS_OK;
}
static pxa_status_t check(void *context,uint64_t epoch,const char *path) {
    directory_picker_t *p=context;
    return pxa_esp_storage_directory_check(p->prompt.prompt_id,epoch,path);
}
static void show(void *context) {
    pxa_esp_ui_directory_prompt_t *prompt=context;
    if(prompt->prompt_id!=g_requested) {free(prompt);return;}
    close_picker();
    directory_picker_t *p=calloc(1,sizeof(*p));
    if(!p) {(void)pxa_esp_storage_choose_directory(prompt->prompt_id,0,"");free(prompt);return;}
    p->prompt=*prompt;free(prompt);g_picker=p;
    p->completion=lv_timer_create(complete,1,p);
    if(!p->completion) {(void)pxa_esp_storage_choose_directory(p->prompt.prompt_id,0,"");g_picker=NULL;free(p);return;}
    lv_timer_pause(p->completion);
    pxa_lvgl_file_picker_config_t config={.context=p,.browse=browse,.check_directory=check,
        .changed=changed,.overlay=overlay,.mode=PXA_FILE_PICKER_DIRECTORY,
        .max_path_bytes=PXA_ESP_STORAGE_PATH_BYTES-1,.extensions="",
        .style=pxa_esp_dialog_theme(),.body_font=pxa_esp_host_ui_body_font(),
        .title_font=pxa_esp_host_ui_title_font(),.caption_font=pxa_esp_host_ui_caption_font(),
        .app_name=p->prompt.app_name,.purpose=p->prompt.purpose,.chinese=pxa_esp_host_locale_is_chinese()};
    p->picker=pxa_lvgl_file_picker_create(&config);
    if(!p->picker) {(void)pxa_esp_storage_choose_directory(p->prompt.prompt_id,0,"");close_picker();}
    else if(g_pending_back==p->prompt.prompt_id) {g_pending_back=0;pxa_lvgl_file_picker_back(p->picker);}
}
static void dismiss(void *context) {
    uint32_t id=(uint32_t)(uintptr_t)context;
    if(g_picker && g_picker->prompt.prompt_id==id)close_picker();
}
static void back(void *context) {
    uint32_t id=(uint32_t)(uintptr_t)context;
    if(id!=g_requested)return;
    if(g_picker && g_picker->prompt.prompt_id==id)pxa_lvgl_file_picker_back(g_picker->picker);
    else g_pending_back=id;
}
int pxa_esp_ui_shell_post_directory_prompt(const pxa_esp_ui_directory_prompt_t *prompt) {
    if(!prompt || !prompt->prompt_id)return 0;
    pxa_esp_ui_directory_prompt_t *copy=malloc(sizeof(*copy));if(!copy)return 0;
    *copy=*prompt;lv_lock();lv_result_t result=lv_async_call(show,copy);
    if(result==LV_RESULT_OK) {g_requested=prompt->prompt_id;g_pending_back=0;}
    lv_unlock();
    if(result!=LV_RESULT_OK)free(copy);
    return result==LV_RESULT_OK;
}
void pxa_esp_ui_shell_dismiss_directory_prompt(uint32_t id) {
    lv_lock();
    if(g_requested==id) {g_requested=0;g_pending_back=0;}
    (void)lv_async_call(dismiss,(void *)(uintptr_t)id);lv_unlock();
}
void pxa_esp_ui_shell_back_directory_prompt(uint32_t id) {
    lv_lock();(void)lv_async_call(back,(void *)(uintptr_t)id);lv_unlock();
}
