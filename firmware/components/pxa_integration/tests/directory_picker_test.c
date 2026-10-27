/* SPDX-License-Identifier: MIT */
/* Exercise the firmware wrapper with real LVGL object and timer lifetimes. */
#include "lvgl.h"
#define ESP_PLATFORM 1
#include "../../pxa/src/ui/pxa_esp_directory_picker.c"
#include <assert.h>
#include <stdio.h>

static lv_obj_t *trusted_overlay;
static unsigned decisions;
static uint32_t decision_id;
static uint64_t decision_epoch;
static char decision_path[256];
static bool volume_ready=true;
void pxa_esp_host_ui_metrics(uint16_t *spacing,uint16_t *radius) {*spacing=4;*radius=4;}
bool pxa_esp_host_locale_is_chinese(void) {return false;}
uint32_t pxa_esp_host_ui_color(uint8_t index) {return 0x102030ffu+(uint32_t)index*0x01010100u;}
const lv_font_t *pxa_esp_host_ui_body_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_host_ui_label_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_host_ui_caption_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_host_ui_title_font(void) {return LV_FONT_DEFAULT;}
void pxa_esp_system_overlay_add(lv_obj_t *object) {assert(!trusted_overlay);trusted_overlay=object;}
void pxa_esp_system_overlay_remove(lv_obj_t *object) {assert(trusted_overlay==object);trusted_overlay=NULL;}
void pxa_esp_system_overlay_refresh(void) {}
size_t pxa_esp_storage_volumes(pxa_volume_info_t *out,size_t capacity) {
    assert(capacity);*out=(pxa_volume_info_t){.mount_epoch=42,.state=PXA_VOLUME_READY,.capabilities=PXA_VOLUME_READ};
    strcpy(out->name,"Card");return 1;
}
pxa_status_t pxa_esp_storage_directory_entries(uint32_t id,uint64_t epoch,const char *path,
    size_t skip,size_t capacity,size_t *count,uint8_t *more,pxa_esp_directory_entry_fn emit,void *context) {
    assert(id && epoch==42 && capacity);*count=0;*more=0;
    if(!volume_ready)return PXA_STATUS_UNAVAILABLE;
    if(skip || *path)return PXA_STATUS_OK;
    const pxa_bytes_t name={(const uint8_t *)"Empty",5};
    pxa_status_t status=emit(context,0,PXA_FS_KIND_DIRECTORY,name);
    if(!status)*count=1;
    return status;
}
pxa_status_t pxa_esp_storage_directory_check(uint32_t id,uint64_t epoch,const char *path) {
    assert(id && epoch==42 && path);return volume_ready?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;
}
bool pxa_esp_storage_choose_directory(uint32_t id,uint64_t epoch,const char *path) {
    ++decisions;decision_id=id;decision_epoch=epoch;strcpy(decision_path,path);return true;
}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels) {
    (void)area;(void)pixels;lv_display_flush_ready(display);
}
static void pump(void) {lv_tick_inc(5);(void)lv_timer_handler();}
static unsigned timer_count(void) {
    unsigned count=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))++count;return count;
}
static lv_obj_t *find_text(lv_obj_t *root,const char *text) {
    if(lv_obj_has_class(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text))return root;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *found=find_text(lv_obj_get_child(root,(int32_t)i),text);if(found)return found;
    }
    return NULL;
}
static void click(const char *text) {
    lv_obj_t *label=find_text(trusted_overlay,text);assert(label);
    lv_obj_send_event(lv_obj_get_parent(label),LV_EVENT_CLICKED,NULL);
}
int main(void) {
    lv_init();lv_display_t *display=lv_display_create(320,240);assert(display);
    static uint8_t pixels[320*16*4];
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    const unsigned baseline=timer_count();
    pxa_esp_ui_directory_prompt_t prompt={.prompt_id=41};
    strcpy(prompt.app_name,"Test app");strcpy(prompt.purpose,"Read games from this folder");
    assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));strcpy(prompt.app_name,"Modified");pump();
    assert(g_picker && trusted_overlay && !strcmp(g_picker->prompt.app_name,"Test app"));
    assert(find_text(trusted_overlay,"Choose folder"));
    click("Card");click("Empty");click("Select folder");
    assert(g_picker && !decisions); /* no deletion inside the click callback */
    pump();assert(!g_picker && !trusted_overlay && decisions==1 && decision_id==41);
    assert(decision_epoch==42 && !strcmp(decision_path,"Empty") && timer_count()==baseline);
    /* System Back climbs one level at a time; root Back cancels the prompt. */
    prompt.prompt_id=42;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));pump();
    click("Card");click("Empty");
    pxa_esp_ui_shell_back_directory_prompt(42);pump();assert(find_text(trusted_overlay,"Empty"));
    pxa_esp_ui_shell_back_directory_prompt(42);pump();assert(find_text(trusted_overlay,"Choose storage"));
    pxa_esp_ui_shell_back_directory_prompt(42);pump();pump();
    assert(!g_picker && decisions==2 && decision_id==42 && !decision_epoch && timer_count()==baseline);
    /* A card removed after browsing must not be accepted. */
    prompt.prompt_id=43;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));pump();click("Card");
    volume_ready=false;click("Select folder");pump();assert(g_picker && decisions==2);
    assert(lv_obj_has_state(lv_obj_get_parent(find_text(trusted_overlay,"Select folder")),LV_STATE_DISABLED));
    click("Cancel");pump();assert(!g_picker && decisions==3 && !decision_epoch);volume_ready=true;
    /* Reverse delivery of show/dismiss/new-show cannot resurrect a prompt. */
    prompt.prompt_id=50;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));
    pxa_esp_ui_shell_dismiss_directory_prompt(50);
    prompt.prompt_id=51;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));
    pxa_esp_ui_shell_dismiss_directory_prompt(51);
    prompt.prompt_id=52;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));pump();
    assert(g_picker && g_picker->prompt.prompt_id==52 && decisions==3);
    pxa_esp_ui_shell_dismiss_directory_prompt(51);pump();assert(g_picker && g_picker->prompt.prompt_id==52);
    pxa_esp_ui_shell_dismiss_directory_prompt(52);pump();assert(!g_picker && !trusted_overlay && timer_count()==baseline);
    /* A queued Back still cancels when delivered before show. */
    prompt.prompt_id=53;assert(pxa_esp_ui_shell_post_directory_prompt(&prompt));
    pxa_esp_ui_shell_back_directory_prompt(53);pump();pump();
    assert(!g_picker && decisions==4 && decision_id==53 && !decision_epoch && timer_count()==baseline);
    pump();lv_display_delete(display);lv_deinit();
    puts("Native directory picker: current/empty folder, Back, removal, copied prompts, stale async ordering and timer cleanup passed");
    return 0;
}
