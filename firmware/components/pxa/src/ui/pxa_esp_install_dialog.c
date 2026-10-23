#include "pxa_esp_ui_shell.h"
#include "pxa_esp_system_overlay.h"
#include "pxa_esp_host.h"
#include "pxa_esp_dialog_style.h"
#include "pxa_esp_install_capacity.h"
#include "pxa/ui.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pxa_esp_ui_install_prompt_t prompt;
    lv_obj_t *mask,*panel,*details,*choice,*media_choice,*install;
    lv_obj_t *progress;
    lv_timer_t *progress_timer;
    uint8_t media_manual,updating,busy;
} install_dialog_t;
static install_dialog_t *g_dialog;
/* Protected by the LVGL lock, including all cross-task post operations. */
static uint32_t g_requested_prompt;
/* Zero is confirmation; busy values identify the exact requested phase. */
static uint8_t g_requested_busy;
static void key(lv_event_t *event);
static lv_color_t color(uint8_t index) {return lv_color_hex(pxa_esp_host_ui_color(index)>>8);}
static const lv_font_t *font(void) {
    const lv_font_t *f=pxa_esp_host_ui_body_font();return f?f:pxa_esp_ui_shell_text_font();
}
static const lv_font_t *caption_font(void) {
    const lv_font_t *f=pxa_esp_host_ui_caption_font();return f?f:font();
}
static void close_dialog(void) {
    install_dialog_t *dialog=g_dialog;if(!dialog)return;
    if(dialog->progress_timer)lv_timer_delete(dialog->progress_timer);
    g_dialog=NULL;pxa_esp_system_overlay_remove(dialog->mask);
    lv_obj_set_hidden(dialog->mask,true);lv_obj_delete(dialog->mask);free(dialog);
}
static void respond(lv_event_t *event) {
    install_dialog_t *dialog=g_dialog;if(!dialog)return;
    bool approve=(uintptr_t)lv_event_get_user_data(event)!=0;
    uint8_t selection=dialog->prompt.selected;
    if(dialog->prompt.media_count)selection|=(uint8_t)(dialog->prompt.media_selected<<3);
    if(pxa_esp_host_respond_install(dialog->prompt.prompt_id,approve,selection))close_dialog();
}
static void update_details(void) {
    install_dialog_t *dialog=g_dialog;if(!dialog)return;
    const pxa_esp_ui_install_volume_t *v=&dialog->prompt.volumes[dialog->prompt.selected];
    const bool zh=pxa_esp_host_locale_is_chinese();
    char app_space[112],media_space[112];
    pxa_esp_install_capacity_text(app_space,sizeof(app_space),v->required_bytes,v->free_bytes,v->free_known,zh);
    bool fits=v->free_known && v->required_bytes<=v->free_bytes;
    if(dialog->prompt.media_count) {
        const uint8_t selected=dialog->prompt.media_selected;
        const pxa_esp_ui_install_volume_t *media=&dialog->prompt.media_volumes[selected];
        fits=fits && (dialog->prompt.media_fit[dialog->prompt.selected]&(1u<<selected))!=0;
        if(dialog->prompt.same_media[dialog->prompt.selected]==selected)
            fits=fits && dialog->prompt.combined_bytes[dialog->prompt.selected][selected]<=v->free_bytes;
        else if(!dialog->prompt.media_reuse[selected])
            fits=fits && media->free_known && media->required_bytes<=media->free_bytes;
        if(!fits)lv_label_set_text(dialog->details,zh?"所选位置空间不足或不可用":"Selected storage is full or unavailable");
        else if(dialog->prompt.same_media[dialog->prompt.selected]==selected) {
            uint64_t total=dialog->prompt.combined_bytes[dialog->prompt.selected][selected];
            pxa_esp_install_capacity_text(app_space,sizeof(app_space),total,v->free_bytes,v->free_known,zh);
            lv_label_set_text_fmt(dialog->details,"%s%s",app_space,
                dialog->prompt.media_reuse[selected]?(zh?" · 复用资源":" · reuse"):"");
        } else if(dialog->prompt.media_reuse[selected])lv_label_set_text_fmt(dialog->details,
            zh?"应用：%s\n资源：复用现有文件":"App: %s\nResources: reuse existing files",app_space);
        else {
            pxa_esp_install_capacity_text(media_space,sizeof(media_space),media->required_bytes,media->free_bytes,media->free_known,zh);
            lv_label_set_text_fmt(dialog->details,zh?"应用：%s\n资源：%s":"App: %s\nResources: %s",app_space,media_space);
        }
    } else lv_label_set_text(dialog->details,app_space);
    if(dialog->install) {
        if(fits)lv_obj_remove_state(dialog->install,LV_STATE_DISABLED);
        else lv_obj_add_state(dialog->install,LV_STATE_DISABLED);
    }
    lv_obj_set_style_text_color(dialog->details,color(fits?PXA_UI_THEME_MUTED:PXA_UI_THEME_DANGER),0);
    pxa_esp_system_overlay_refresh();
}
static void selection(lv_event_t *event) {
    install_dialog_t *dialog=g_dialog;if(!dialog || dialog->updating)return;
    lv_obj_t *target=(lv_obj_t *)lv_event_get_current_target(event);
    uint32_t value=lv_dropdown_get_selected(target);
    if(target==dialog->media_choice) {
        if(value>=dialog->prompt.media_count)return;
        dialog->media_manual=1;dialog->prompt.media_selected=(uint8_t)value;
    } else {
        if(value>=dialog->prompt.count)return;
        dialog->prompt.selected=(uint8_t)value;
        if(dialog->prompt.media_count && !dialog->media_manual) {
            uint8_t media=dialog->prompt.same_media[value];
            if(media<dialog->prompt.media_count && (dialog->prompt.media_fit[value]&(1u<<media))) {
                dialog->prompt.media_selected=media;dialog->updating=1;
                lv_dropdown_set_selected(dialog->media_choice,media);dialog->updating=0;
            }
        }
    }
    update_details();
}
static void refresh(void *context) {(void)context;pxa_esp_system_overlay_refresh();}
static void popup(lv_event_t *event) {
    if(!g_dialog)return;
    if(lv_event_get_code(event)==LV_EVENT_READY) {
        /* LVGL normally reparents popup lists beside the modal root. Keep
         * this list inside the trusted overlay's input and snapshot tree. */
        lv_obj_t *list=lv_dropdown_get_list((lv_obj_t *)lv_event_get_current_target(event));
        lv_obj_update_layout(list);lv_area_t area;lv_obj_get_coords(list,&area);
        lv_obj_set_parent(list,g_dialog->mask);lv_obj_move_to_index(list,-1);
        const int32_t margin=pxa_esp_dialog_px(12);
        int32_t width=lv_obj_get_width(g_dialog->mask),height=lv_obj_get_height(g_dialog->mask);
        lv_obj_set_style_max_width(list,width-2*margin,0);
        lv_obj_set_style_max_height(list,height-2*margin,0);lv_obj_update_layout(list);
        lv_obj_set_pos(list,pxa_esp_dialog_limit(area.x1,margin,width-margin-lv_obj_get_width(list)),
            pxa_esp_dialog_limit(area.y1,margin,height-margin-lv_obj_get_height(list)));
        (void)lv_async_call(refresh,NULL);
    } else pxa_esp_system_overlay_refresh();
}
static void scrolled(lv_event_t *event) {(void)event;pxa_esp_system_overlay_refresh();}
static lv_obj_t *dropdown(lv_obj_t *parent,const pxa_esp_ui_install_volume_t *volumes,uint8_t count,uint8_t selected) {
    lv_obj_t *choice=lv_dropdown_create(parent);lv_obj_set_width(choice,LV_PCT(100));
    lv_obj_set_style_text_font(choice,font(),0);pxa_esp_dialog_style_dropdown(choice);
    lv_dropdown_clear_options(choice);
    for(uint8_t i=0;i<count;++i)lv_dropdown_add_option(choice,volumes[i].name,LV_DROPDOWN_POS_LAST);
    lv_dropdown_set_selected(choice,selected);
    lv_obj_t *list=lv_dropdown_get_list(choice);lv_obj_set_style_text_font(list,font(),0);
    if(count==1) {
        /* A single fitting location is readable, not unavailable. */
        lv_dropdown_set_symbol(choice,NULL);lv_obj_set_clickable(choice,false);lv_obj_set_click_focusable(choice,false);
    }
    lv_obj_add_event_cb(choice,selection,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(choice,key,LV_EVENT_KEY|LV_EVENT_PREPROCESS,NULL);
    lv_obj_add_event_cb(choice,popup,LV_EVENT_READY,NULL);lv_obj_add_event_cb(choice,popup,LV_EVENT_CANCEL,NULL);
    lv_obj_add_event_cb(list,scrolled,LV_EVENT_SCROLL,NULL);return choice;
}
static lv_obj_t *location_row(lv_obj_t *parent,const char *text) {
    lv_obj_t *row=lv_obj_create(parent);lv_obj_remove_style_all(row);
    lv_obj_set_size(row,LV_PCT(100),pxa_esp_dialog_px(36));lv_obj_set_scrollable(row,false);
    lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);lv_obj_set_flex_align(row,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row,pxa_esp_dialog_spacing(2),0);lv_obj_t *label=lv_label_create(row);
    lv_obj_set_style_text_color(label,color(PXA_UI_THEME_MUTED),0);lv_obj_set_width(label,LV_PCT(36));
    lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);lv_label_set_text(label,text);return row;
}
static void key(lv_event_t *event) {
    if(g_dialog && g_dialog->busy)return;
    if(lv_event_get_key(event)==LV_KEY_ESC) {
        lv_obj_t *target=lv_event_get_current_target(event);
        if(g_dialog && (target==g_dialog->choice || target==g_dialog->media_choice) &&
            lv_dropdown_is_open(target))return; /* First Escape closes only the list. */
        if(g_dialog && pxa_esp_host_respond_install(g_dialog->prompt.prompt_id,false,g_dialog->prompt.selected))close_dialog();
        lv_event_stop_bubbling(event);
    }
}
static void show(void *context) {
    pxa_esp_ui_install_prompt_t *prompt=context;
    if(g_requested_prompt!=prompt->prompt_id || g_requested_busy) {free(prompt);return;}
    close_dialog();
    install_dialog_t *dialog=heap_caps_calloc(1,sizeof(*dialog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!dialog) {(void)pxa_esp_host_respond_install(prompt->prompt_id,false,0);free(prompt);return;}
    dialog->prompt=*prompt;free(prompt);g_dialog=dialog;const bool zh=pxa_esp_host_locale_is_chinese();
    dialog->mask=lv_obj_create(lv_layer_top());pxa_esp_dialog_style_mask(dialog->mask);
    /* Escape belongs to the controls. A KEY listener on the full-screen
     * scrim makes keypad navigation select and outline that scrim first. */
    lv_obj_t *box=lv_obj_create(dialog->mask);dialog->panel=box;pxa_esp_dialog_style_panel(box);
    lv_obj_set_size(box,pxa_esp_dialog_limit(lv_display_get_horizontal_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(400)),
        pxa_esp_dialog_limit(lv_display_get_vertical_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(360)));
    lv_obj_center(box);lv_obj_set_flex_flow(box,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_font(box,font(),0);
    lv_obj_t *title=lv_label_create(box);lv_obj_set_width(title,LV_PCT(100));
    lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
    const lv_font_t *heading=pxa_esp_host_ui_title_font();if(heading)lv_obj_set_style_text_font(title,heading,0);
    lv_label_set_text_fmt(title,"%s · %s",dialog->prompt.app_name,
        dialog->prompt.legacy_upgrade?(zh?"升级存储":"Storage upgrade"):
        dialog->prompt.move?(zh?"移动":"Move"):dialog->prompt.version);
    lv_obj_t *content=lv_obj_create(box);lv_obj_set_width(content,LV_PCT(100));lv_obj_set_flex_grow(content,1);
    lv_obj_set_style_pad_all(content,0,0);lv_obj_set_style_bg_opa(content,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(content,0,0);lv_obj_set_flex_flow(content,LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_text_color(content,color(PXA_UI_THEME_TEXT),0);
    lv_obj_set_style_pad_row(content,pxa_esp_dialog_limit(pxa_esp_dialog_spacing(1)/2,1,LV_COORD_MAX),0);
    lv_obj_set_style_pad_right(content,pxa_esp_dialog_px(6),0);
    lv_obj_set_scroll_dir(content,LV_DIR_VER);pxa_esp_dialog_style_scrollbar(content);
    lv_obj_add_event_cb(content,scrolled,LV_EVENT_SCROLL,NULL);
    lv_obj_t *label=lv_label_create(content);lv_obj_set_style_text_color(label,color(PXA_UI_THEME_MUTED),0);
    lv_obj_set_style_text_font(label,caption_font(),0);
    lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
    if(dialog->prompt.legacy_upgrade) {
        lv_label_set_text(label,zh?"统一代码、存档与权限的存储布局":"Unify code, saves and permission storage");
    } else if(dialog->prompt.move) {
        lv_label_set_text_fmt(label,zh?"当前位置：%s":"Current storage: %s",dialog->prompt.current_volume);
    } else if(dialog->prompt.resources_only)lv_label_set_text_fmt(label,zh?"游戏资源 · 发布者 %.12s…":"Game resources · Publisher %.12s…",dialog->prompt.publisher);
    else lv_label_set_text_fmt(label,zh?"发布者 %.12s… · %u 权限":"Publisher %.12s… · %u permissions",
        dialog->prompt.publisher,(unsigned)dialog->prompt.permission_count);
    if(dialog->prompt.move && dialog->prompt.retained_media_volume[0]) {
        label=lv_label_create(content);lv_obj_set_style_text_color(label,color(PXA_UI_THEME_MUTED),0);
        lv_obj_set_width(label,LV_PCT(100));lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label,caption_font(),0);
        lv_label_set_text_fmt(label,dialog->prompt.resources_only?
            (zh?"应用与数据保留在 %s":"App and data stay on %s"):
            (zh?"资源保留在 %s":"Resources stay on %s"),dialog->prompt.retained_media_volume);
    }
    if(dialog->prompt.media_count) {
        lv_obj_t *row=location_row(content,zh?"应用与数据":"App & data");
        dialog->choice=dropdown(row,dialog->prompt.volumes,dialog->prompt.count,dialog->prompt.selected);
        lv_obj_set_width(dialog->choice,0);lv_obj_set_height(dialog->choice,pxa_esp_dialog_px(36));lv_obj_set_flex_grow(dialog->choice,1);
        row=location_row(content,zh?"资源":"Resources");
        dialog->media_choice=dropdown(row,dialog->prompt.media_volumes,dialog->prompt.media_count,dialog->prompt.media_selected);
        lv_obj_set_width(dialog->media_choice,0);lv_obj_set_height(dialog->media_choice,pxa_esp_dialog_px(36));lv_obj_set_flex_grow(dialog->media_choice,1);
    } else if(dialog->prompt.legacy_upgrade) {
        lv_obj_t *row=location_row(content,zh?"当前位置":"Storage");
        dialog->choice=dropdown(row,dialog->prompt.volumes,dialog->prompt.count,dialog->prompt.selected);
        lv_obj_set_width(dialog->choice,0);lv_obj_set_height(dialog->choice,pxa_esp_dialog_px(36));
        lv_obj_set_flex_grow(dialog->choice,1);
    } else {
        label=lv_label_create(content);lv_obj_set_style_text_color(label,color(PXA_UI_THEME_TEXT),0);
        lv_label_set_text(label,dialog->prompt.move?(dialog->prompt.resources_only?
            (zh?"资源移至":"Move resources to"):(zh?"应用与数据移至":"Move app and data to")):
            dialog->prompt.download_only?(zh?"下载到":"Download to"):
            dialog->prompt.resources_only?(zh?"资源安装位置":"Resource storage"):(zh?"安装应用与数据到":"App and data storage"));
        dialog->choice=dropdown(content,dialog->prompt.volumes,dialog->prompt.count,dialog->prompt.selected);
    }
    dialog->details=lv_label_create(content);lv_obj_set_style_text_color(dialog->details,color(PXA_UI_THEME_MUTED),0);lv_obj_set_width(dialog->details,LV_PCT(100));
    lv_obj_set_style_text_font(dialog->details,caption_font(),0);
    lv_label_set_long_mode(dialog->details,LV_LABEL_LONG_WRAP);
    lv_obj_t *footer=lv_obj_create(box);lv_obj_remove_style_all(footer);lv_obj_set_width(footer,LV_PCT(100));
    lv_obj_set_height(footer,pxa_esp_dialog_px(36));lv_obj_set_flex_flow(footer,LV_FLEX_FLOW_ROW);lv_obj_set_style_pad_column(footer,pxa_esp_dialog_spacing(2),0);
    for(unsigned i=0;i<2;++i) {
        lv_obj_t *button=lv_button_create(footer);lv_obj_set_width(button,0);lv_obj_set_flex_grow(button,1);lv_obj_set_height(button,pxa_esp_dialog_px(36));
        if(i)dialog->install=button;
        pxa_esp_dialog_style_button(button,i!=0);
        lv_obj_t *text=lv_label_create(button);lv_label_set_text(text,i?(dialog->prompt.legacy_upgrade?(zh?"升级":"Upgrade"):
            dialog->prompt.move?(zh?"移动":"Move"):
            dialog->prompt.download_only?(zh?"下载":"Download"):(zh?"安装":"Install")):(zh?"取消":"Cancel"));
        lv_obj_set_width(text,LV_PCT(100));lv_label_set_long_mode(text,LV_LABEL_LONG_DOT);
        lv_obj_center(text);lv_obj_add_event_cb(button,respond,LV_EVENT_CLICKED,(void *)(uintptr_t)i);
        lv_obj_add_event_cb(button,key,LV_EVENT_KEY|LV_EVENT_PREPROCESS,NULL);
    }
    pxa_esp_system_overlay_add(dialog->mask);update_details();
}
static void dismiss(void *context) {
    if(g_dialog && g_dialog->prompt.prompt_id==(uint32_t)(uintptr_t)context)close_dialog();
}
int pxa_esp_ui_shell_post_install_prompt(const pxa_esp_ui_install_prompt_t *prompt) {
    if(!prompt || !prompt->prompt_id || !prompt->count || prompt->count>8 || prompt->selected>=prompt->count ||
        prompt->media_count>8 || (prompt->media_count && prompt->media_selected>=prompt->media_count))return 0;
    pxa_esp_ui_install_prompt_t *copy=heap_caps_malloc(sizeof(*copy),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!copy)return 0;
    *copy=*prompt;
    lv_lock();g_requested_prompt=prompt->prompt_id;g_requested_busy=0;
    lv_result_t result=lv_async_call(show,copy);
    if(result!=LV_RESULT_OK)g_requested_prompt=0;
    lv_unlock();
    if(result!=LV_RESULT_OK)free(copy);
    return result==LV_RESULT_OK;
}
void pxa_esp_ui_shell_dismiss_install_prompt(uint32_t id) {
    lv_lock();if(g_requested_prompt==id)g_requested_prompt=0;
    (void)lv_async_call(dismiss,(void *)(uintptr_t)id);lv_unlock();
}

enum { PROGRESS_MOVE,PROGRESS_INSTALL,PROGRESS_VERIFY };
typedef struct {uint32_t id;int32_t status;char name[64];uint8_t install,resources_only;} move_notice_t;
static void advance_progress(lv_timer_t *timer) {
    install_dialog_t *dialog=lv_timer_get_user_data(timer);
    if(dialog!=g_dialog || !dialog->busy || !dialog->progress)return;
    lv_arc_set_rotation(dialog->progress,(lv_arc_get_rotation(dialog->progress)+45u)%360u);
}
static void move_notice(void *context) {
    move_notice_t *notice=context;const bool zh=pxa_esp_host_locale_is_chinese();
    if(notice->name[0]) {
        if(g_requested_prompt!=notice->id || g_requested_busy!=notice->install+1u) {free(notice);return;}
        close_dialog();
        install_dialog_t *dialog=heap_caps_calloc(1,sizeof(*dialog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(dialog) {
            g_dialog=dialog;dialog->busy=1;dialog->prompt.prompt_id=notice->id;dialog->prompt.move=!notice->install;dialog->prompt.resources_only=notice->resources_only;
            dialog->mask=lv_obj_create(lv_layer_top());pxa_esp_dialog_style_mask(dialog->mask);
            lv_obj_t *box=lv_obj_create(dialog->mask);dialog->panel=box;
            lv_obj_set_size(box,pxa_esp_dialog_limit(lv_display_get_horizontal_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(400)),
                pxa_esp_dialog_limit(lv_display_get_vertical_resolution(lv_display_get_default())-pxa_esp_dialog_px(24),1,pxa_esp_dialog_px(148)));
            lv_obj_center(box);pxa_esp_dialog_style_panel(box);
            lv_obj_set_style_text_font(box,font(),0);lv_obj_set_style_text_color(box,color(PXA_UI_THEME_TEXT),0);
            lv_obj_t *title=lv_label_create(box);lv_obj_set_width(title,LV_PCT(100));lv_label_set_long_mode(title,LV_LABEL_LONG_DOT);
            const lv_font_t *heading=pxa_esp_host_ui_title_font();if(heading)lv_obj_set_style_text_font(title,heading,0);
            lv_label_set_text(title,notice->name);lv_obj_align(title,LV_ALIGN_TOP_MID,0,0);
            /* A copy shares the runtime core with the higher-priority LVGL
             * task. Keep its waiting indicator at 8 Hz instead of requesting
             * a new animated frame on every display refresh. */
            dialog->progress=lv_arc_create(box);lv_obj_set_size(dialog->progress,pxa_esp_dialog_px(30),pxa_esp_dialog_px(30));lv_obj_center(dialog->progress);
            lv_obj_set_clickable(dialog->progress,false);lv_obj_set_scrollable(dialog->progress,false);
            lv_obj_remove_style(dialog->progress,NULL,LV_PART_KNOB);
            lv_obj_set_style_arc_opa(dialog->progress,LV_OPA_TRANSP,LV_PART_MAIN);
            lv_obj_set_style_arc_color(dialog->progress,color(PXA_UI_THEME_PRIMARY),LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(dialog->progress,3,LV_PART_INDICATOR);
            lv_arc_set_angles(dialog->progress,0,90);
            dialog->progress_timer=lv_timer_create(advance_progress,125,dialog);
            lv_obj_t *label=lv_label_create(box);lv_obj_set_width(label,LV_PCT(100));
            lv_obj_set_style_text_font(label,caption_font(),0);lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_text_color(label,color(PXA_UI_THEME_MUTED),0);
            lv_label_set_text(label,notice->install==PROGRESS_VERIFY?(zh?"正在校验安装包…":"Checking package…"):
                notice->install?(notice->resources_only?
                (zh?"正在安装资源…":"Installing resources…"):(zh?"正在安装应用…":"Installing app…")):
                notice->resources_only?(zh?"正在移动资源…":"Moving resources…"):
                (zh?"正在移动应用与数据…":"Moving app and data…"));lv_obj_align(label,LV_ALIGN_BOTTOM_MID,0,0);
            pxa_esp_system_overlay_add(dialog->mask);
        }
    } else {
        if(g_dialog && g_dialog->prompt.prompt_id==notice->id)close_dialog();
        const char *text=notice->install?(notice->status==PXA_STATUS_OK?(zh?"安装完成":"Installation complete"):
            notice->status==PXA_STATUS_QUOTA_EXCEEDED?(zh?"存储空间不足":"Not enough storage"):
            notice->status==PXA_STATUS_BAD_STATE?(zh?"安装状态已变化，请重新确认":"Install state changed; confirm again"):
            (zh?"安装未完成，请检查文件与存储设备":"Install incomplete; check file and storage")):
            notice->status==PXA_STATUS_OK?(zh?"存储位置已更新":"Storage location updated"):
            notice->status==PXA_STATUS_QUOTA_EXCEEDED?(zh?"存储空间不足":"Not enough storage"):
            notice->status==PXA_STATUS_BAD_STATE?(zh?"应用状态已变化，请重新选择移动位置":"App state changed; choose storage again"):
            notice->status==PXA_STATUS_DENIED?(zh?"校验未通过，移动未完成":"Verification failed; move incomplete"):
            (zh?"移动未完成，请检查存储设备后重试":"Move incomplete; check storage and retry");
        pxa_esp_ui_shell_post_toast(text,3000);
    }
    free(notice);
}
static int post_progress(uint32_t id,const char *name,uint8_t install,uint8_t resources_only) {
    move_notice_t *notice=heap_caps_calloc(1,sizeof(*notice),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!notice)return 0;
    notice->id=id;notice->install=install;notice->resources_only=resources_only;
    snprintf(notice->name,sizeof(notice->name),"%s",name);
    lv_lock();g_requested_prompt=id;g_requested_busy=install+1u;
    lv_result_t result=lv_async_call(move_notice,notice);
    if(result!=LV_RESULT_OK)g_requested_prompt=0;
    lv_unlock();
    if(result!=LV_RESULT_OK)free(notice);
    return result==LV_RESULT_OK;
}
static void post_result(uint32_t id,int32_t status,uint8_t install) {
    move_notice_t *notice=heap_caps_calloc(1,sizeof(*notice),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!notice) {pxa_esp_ui_shell_dismiss_install_prompt(id);return;}
    notice->id=id;notice->status=status;notice->install=install;
    lv_lock();if(g_requested_prompt==id)g_requested_prompt=0;
    lv_result_t result=lv_async_call(move_notice,notice);lv_unlock();
    if(result!=LV_RESULT_OK) {free(notice);pxa_esp_ui_shell_dismiss_install_prompt(id);}
}
int pxa_esp_ui_shell_post_move_progress(uint32_t id,const char *name) {return post_progress(id,name,PROGRESS_MOVE,0);}
int pxa_esp_ui_shell_post_move_progress_kind(uint32_t id,const char *name,uint8_t resources_only) {
    return post_progress(id,name,PROGRESS_MOVE,resources_only);
}
void pxa_esp_ui_shell_post_move_result(uint32_t id,int32_t status) {post_result(id,status,0);}
int pxa_esp_ui_shell_post_file_install_progress(uint32_t id,const char *name,uint8_t resources_only) {
    return post_progress(id,name,PROGRESS_INSTALL,resources_only);
}
int pxa_esp_ui_shell_post_file_prepare_progress(uint32_t id,const char *name) {
    return post_progress(id,name,PROGRESS_VERIFY,0);
}
void pxa_esp_ui_shell_post_file_install_result(uint32_t id,int32_t status) {post_result(id,status,1);}
