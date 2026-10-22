#include "pxa_esp_ui_shell.h"
#include "pxa_esp_system_overlay.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa_esp_host.h"
#include "pxa_esp_dialog_style.h"
#include "pxa/ui.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLDER_PAGE 12u
typedef struct {
    pxa_esp_ui_directory_prompt_t prompt;
    lv_obj_t *mask,*list,*path_label,*use;
    pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];
    pxa_esp_storage_folder_t folders[FOLDER_PAGE];
    size_t volume_count,folder_count;
    uint64_t epoch;
    uint32_t skip;
    char relative[PXA_ESP_STORAGE_PATH_BYTES];
} picker_t;
static picker_t *g_picker;
static lv_color_t color(uint8_t index) { return lv_color_hex(pxa_esp_host_ui_color(index)>>8); }
static const lv_font_t *body_font(void) {
    const lv_font_t *font=pxa_esp_host_ui_body_font();
    return font?font:pxa_esp_ui_shell_text_font();
}
static const lv_font_t *caption_font(void) {
    const lv_font_t *font=pxa_esp_host_ui_caption_font();
    return font?font:body_font();
}
static void render(picker_t *p);
static void close_picker(void) {
    picker_t *p=g_picker;
    if(!p) return;
    g_picker=NULL;
    pxa_esp_system_overlay_remove(p->mask);
    lv_obj_add_flag(p->mask,LV_OBJ_FLAG_HIDDEN);
    /* Remove callbacks that borrow p before returning its memory. */
    lv_obj_delete(p->mask); free(p);
}
static void cancel(lv_event_t *e) {
    picker_t *p=g_picker; (void)e;
    if(p && pxa_esp_storage_choose_directory(p->prompt.prompt_id,0,"")) close_picker();
}
static void select_folder(lv_event_t *e) {
    picker_t *p=g_picker; (void)e;
    if(p && p->epoch && pxa_esp_storage_choose_directory(p->prompt.prompt_id,
        p->epoch,p->relative)) close_picker();
}
static void back(lv_event_t *e) {
    picker_t *p=g_picker; char *slash; (void)e;
    if(!p) return;
    p->skip=0;
    if(!p->epoch) { cancel(e); return; }
    if(!*p->relative) p->epoch=0;
    else if((slash=strrchr(p->relative,'/'))) *slash=0;
    else *p->relative=0;
    render(p);
}
static void key(lv_event_t *e) {
    if(lv_event_get_key(e)==LV_KEY_ESC) { lv_event_stop_bubbling(e); back(e); }
}
static void open_volume(lv_event_t *e) {
    size_t i=(size_t)(uintptr_t)lv_event_get_user_data(e); picker_t *p=g_picker;
    if(!p || i>=p->volume_count) return;
    p->epoch=p->volumes[i].mount_epoch; p->skip=0; p->relative[0]=0; render(p);
}
static void open_folder(lv_event_t *e) {
    size_t i=(size_t)(uintptr_t)lv_event_get_user_data(e); picker_t *p=g_picker;
    char next[PXA_ESP_STORAGE_PATH_BYTES];
    if(!p || i>=p->folder_count) return;
    int n=snprintf(next,sizeof(next),"%s%s%s",p->relative,*p->relative?"/":"",p->folders[i].name);
    if(n<0 || (size_t)n>=sizeof(next)) return;
    strcpy(p->relative,next); p->skip=0; render(p);
}
static void next_page(lv_event_t *e) {
    picker_t *p=g_picker; (void)e;
    if(p) { p->skip+=(uint32_t)p->folder_count; render(p); }
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,lv_event_cb_t callback,void *data) {
    lv_obj_t *b=lv_button_create(parent),*label=lv_label_create(b);
    lv_obj_add_flag(b,LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_height(b,pxa_esp_dialog_px(36));pxa_esp_dialog_style_button(b,false);
    lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
    const lv_font_t *action_font=pxa_esp_host_ui_label_font();
    lv_obj_set_height(label,(action_font?action_font:body_font())->line_height);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(label,text);lv_obj_center(label);
    lv_obj_add_event_cb(b,callback,LV_EVENT_CLICKED,data);
    lv_obj_add_event_cb(b,key,LV_EVENT_KEY,NULL);return b;
}
static void folder_row(lv_obj_t *parent,const char *text,lv_event_cb_t callback,void *data) {
    lv_obj_t *row=button(parent,text,callback,data);
    pxa_esp_dialog_style_list_item(row);lv_obj_set_width(row,LV_PCT(100));
    lv_obj_t *label=lv_obj_get_child(row,0);
    lv_obj_set_style_text_font(row,body_font(),0);
    lv_obj_set_height(label,body_font()->line_height);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_LEFT,0);
    lv_obj_align(label,LV_ALIGN_LEFT_MID,0,0);
}
static void render(picker_t *p) {
    uint8_t more=0;
    lv_obj_clean(p->list);
    if(!p->epoch) {
        lv_label_set_text(p->path_label,pxa_esp_host_locale_is_chinese()?"选择存储":"Choose storage");
        lv_obj_add_state(p->use,LV_STATE_DISABLED);
        p->volume_count=pxa_esp_storage_volumes(p->volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
        for(size_t i=0;i<p->volume_count;++i) {
            pxa_volume_info_t *v=&p->volumes[i];
            if(v->ambiguous || v->state!=PXA_VOLUME_READY) continue;
            folder_row(p->list,v->name,open_volume,(void *)(uintptr_t)i);
        }
    } else {
        lv_label_set_text_fmt(p->path_label,"/%s",p->relative);
        lv_obj_remove_state(p->use,LV_STATE_DISABLED);
        pxa_status_t status=pxa_esp_storage_folders(p->epoch,p->relative,p->skip,
            p->folders,FOLDER_PAGE,&p->folder_count,&more);
        if(status!=PXA_STATUS_OK) {
            lv_obj_t *label=lv_label_create(p->list);
            lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(label,caption_font(),0);
            lv_obj_set_style_text_color(label,color(PXA_UI_THEME_DANGER),0);
            lv_label_set_text(label,pxa_esp_host_locale_is_chinese()?"目录不可用，请返回重新选择":"Folder unavailable. Go back and choose again.");
            lv_obj_add_state(p->use,LV_STATE_DISABLED);
        } else {
            for(size_t i=0;i<p->folder_count;++i) {
                folder_row(p->list,p->folders[i].name,open_folder,(void *)(uintptr_t)i);
            }
            if(more) {
                folder_row(p->list,pxa_esp_host_locale_is_chinese()?"下一页":"Next page",next_page,NULL);
            }
        }
    }
    pxa_esp_system_overlay_refresh();
}
static void show(void *context) {
    pxa_esp_ui_directory_prompt_t *prompt=context;
    close_picker();
    picker_t *p=calloc(1,sizeof(*p));
    if(!p) { (void)pxa_esp_storage_choose_directory(prompt->prompt_id,0,""); free(prompt); return; }
    p->prompt=*prompt; free(prompt); g_picker=p;
    p->mask=lv_obj_create(lv_layer_top());pxa_esp_dialog_style_mask(p->mask);
    lv_obj_t *box=lv_obj_create(p->mask);
    lv_obj_set_size(box,pxa_esp_dialog_limit(lv_display_get_horizontal_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(400)),
        pxa_esp_dialog_limit(lv_display_get_vertical_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(360)));
    lv_obj_center(box);pxa_esp_dialog_style_panel(box);
    lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_font(box,body_font(),0);
    lv_obj_add_flag(box,LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *title=lv_label_create(box);
    lv_obj_set_width(title,LV_PCT(100)); lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    const lv_font_t *heading=pxa_esp_host_ui_title_font();if(!heading)heading=body_font();
    lv_obj_set_style_text_font(title,heading,0);lv_obj_set_height(title,heading->line_height);
    lv_label_set_text_fmt(title,"%s · %s",p->prompt.app_name,
        pxa_esp_host_locale_is_chinese()?"只读":"Read only");
    lv_obj_t *purpose=lv_label_create(box);
    lv_obj_set_width(purpose,LV_PCT(100)); lv_label_set_long_mode(purpose,LV_LABEL_LONG_DOT);
    lv_label_set_text(purpose,p->prompt.purpose);
    lv_obj_set_style_text_font(purpose,caption_font(),0);
    lv_obj_set_style_text_color(purpose,color(PXA_UI_THEME_MUTED),0);
    lv_obj_set_height(purpose,caption_font()->line_height);
    p->path_label=lv_label_create(box); lv_obj_set_width(p->path_label,LV_PCT(100));
    lv_label_set_long_mode(p->path_label,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(p->path_label,caption_font(),0);
    lv_obj_set_style_text_color(p->path_label,color(PXA_UI_THEME_MUTED),0);
    lv_obj_set_height(p->path_label,caption_font()->line_height);
    p->list=lv_obj_create(box); lv_obj_set_width(p->list,LV_PCT(100));
    lv_obj_set_flex_grow(p->list,1); lv_obj_set_flex_flow(p->list,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(p->list,pxa_esp_dialog_spacing(1),0);
    lv_obj_set_style_pad_row(p->list,pxa_esp_dialog_spacing(1),0);
    lv_obj_set_style_bg_color(p->list,color(PXA_UI_THEME_SURFACE_CONTAINER),0);
    lv_obj_set_style_border_width(p->list,0,0);
    lv_obj_set_style_radius(p->list,pxa_esp_dialog_radius(2),0);
    lv_obj_set_scroll_dir(p->list,LV_DIR_VER);pxa_esp_dialog_style_scrollbar(p->list);
    lv_obj_add_flag(p->list,LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *footer=lv_obj_create(box); lv_obj_remove_style_all(footer);
    lv_obj_add_flag(footer,LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(footer,LV_PCT(100));lv_obj_set_height(footer,pxa_esp_dialog_px(36));
    lv_obj_set_flex_flow(footer,LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(footer,pxa_esp_dialog_spacing(2),0);
    const bool chinese=pxa_esp_host_locale_is_chinese();
    button(footer,chinese?"取消":"Cancel",cancel,NULL); button(footer,chinese?"返回":"Back",back,NULL);
    p->use=button(footer,chinese?"选择":"Select",select_folder,NULL);
    pxa_esp_dialog_style_button(p->use,true);
    for(unsigned i=0;i<3;++i) {
        lv_obj_t *action=lv_obj_get_child(footer,i);lv_obj_set_width(action,0);lv_obj_set_flex_grow(action,1);
    }
    pxa_esp_system_overlay_add(p->mask); render(p);
}
static void dismiss(void *context) {
    if(g_picker && g_picker->prompt.prompt_id==(uint32_t)(uintptr_t)context) close_picker();
}
int pxa_esp_ui_shell_post_directory_prompt(const pxa_esp_ui_directory_prompt_t *prompt) {
    pxa_esp_ui_directory_prompt_t *copy=malloc(sizeof(*copy));
    if(!copy) return 0;
    *copy=*prompt; lv_lock(); lv_result_t result=lv_async_call(show,copy); lv_unlock();
    if(result!=LV_RESULT_OK) free(copy);
    return result==LV_RESULT_OK;
}
void pxa_esp_ui_shell_dismiss_directory_prompt(uint32_t id) {
    lv_lock(); (void)lv_async_call(dismiss,(void *)(uintptr_t)id); lv_unlock();
}
