/* Native installer UI against real LVGL timers and object lifetimes. */
#include "lvgl.h"
#include "src/misc/lv_event_private.h"
#define ESP_PLATFORM 1
#include "../../pxa/src/ui/pxa_esp_install_dialog.c"
#include <assert.h>
#include <string.h>

static lv_obj_t *trusted_overlay;
static unsigned decisions,toasts;
static bool accepted;
static uint32_t decision_id;
static bool light_palette,chinese_locale;
static uint16_t theme_spacing,theme_radius;
void pxa_esp_host_ui_metrics(uint16_t *spacing,uint16_t *radius) {
    *spacing=theme_spacing;*radius=theme_radius;
}
void *heap_caps_malloc(size_t bytes,unsigned caps) {(void)caps;return malloc(bytes);}
void *heap_caps_calloc(size_t count,size_t bytes,unsigned caps) {(void)caps;return calloc(count,bytes);}
bool pxa_esp_host_locale_is_chinese(void) {return chinese_locale;}
uint32_t pxa_esp_host_ui_color(uint8_t index) {
    return (light_palette?0xb0c0d0ffu:0x102030ffu)+(uint32_t)index*0x01010100u;
}
const lv_font_t *pxa_esp_host_ui_body_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_host_ui_label_font(void) {return &lv_font_montserrat_14;}
const lv_font_t *pxa_esp_host_ui_caption_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_host_ui_title_font(void) {return LV_FONT_DEFAULT;}
const lv_font_t *pxa_esp_ui_shell_text_font(void) {return LV_FONT_DEFAULT;}
bool pxa_esp_host_respond_install(uint32_t id,bool approve,uint8_t choice) {
    assert(choice==0);++decisions;accepted=approve;decision_id=id;return true;
}
void pxa_esp_ui_shell_post_toast(const char *text,uint32_t duration) {assert(text && duration);++toasts;}
void pxa_esp_system_overlay_add(lv_obj_t *object) {assert(!trusted_overlay);trusted_overlay=object;}
void pxa_esp_system_overlay_remove(lv_obj_t *object) {assert(trusted_overlay==object);trusted_overlay=NULL;}
void pxa_esp_system_overlay_refresh(void) {}
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels) {
    (void)area;(void)pixels;lv_display_flush_ready(display);
}
static void pump(uint32_t elapsed) {lv_tick_inc(elapsed);(void)lv_timer_handler();}
static unsigned timer_count(void) {
    unsigned count=0;for(lv_timer_t *timer=lv_timer_get_next(NULL);timer;timer=lv_timer_get_next(timer))++count;return count;
}
static lv_obj_t *label_with_text(lv_obj_t *parent,const char *text) {
    for(uint32_t i=0;i<lv_obj_get_child_count(parent);++i) {
        lv_obj_t *child=lv_obj_get_child(parent,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class) && !strcmp(lv_label_get_text(child),text))return child;
        lv_obj_t *found=label_with_text(child,text);if(found)return found;
    }
    return NULL;
}
int main(void) {
    pxa_esp_ui_install_volume_t capacity={0};
    pxa_volume_info_t volume={.state=PXA_VOLUME_READY,.space_known=PXA_VOLUME_FREE_KNOWN,
        .free_bytes=2000,.reserved_bytes=250};
    pxa_esp_install_volume_capacity(&capacity,&volume);
    assert(capacity.free_known && capacity.free_bytes==1750);
    volume.reserved_bytes=2001;pxa_esp_install_volume_capacity(&capacity,&volume);
    assert(capacity.free_known && capacity.free_bytes==0);
    volume.space_known=0;pxa_esp_install_volume_capacity(&capacity,&volume);
    assert(!capacity.free_known && !capacity.free_bytes);
    pxa_esp_install_volume_capacity(&capacity,NULL);assert(!capacity.free_known);
    char extreme[112];
    pxa_esp_install_capacity_text(extreme,sizeof(extreme),UINT64_MAX,UINT64_MAX,true,false);
    assert(!strcmp(extreme,"Need 16777216.00 TiB / Free 16777216.00 TiB"));
    lv_init();lv_display_t *display=lv_display_create(320,240);
    static uint8_t pixels[320*16*2];lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    const unsigned baseline=timer_count();
    pxa_esp_ui_install_prompt_t prompt={.prompt_id=41,.count=1,.move=1};
    strcpy(prompt.app_name,"Quake");strcpy(prompt.current_volume,"SD");strcpy(prompt.retained_media_volume,"SD");
    strcpy(prompt.volumes[0].name,"Flash");prompt.volumes[0].required_bytes=3000000;prompt.volumes[0].free_bytes=4000000;prompt.volumes[0].free_known=1;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));strcpy(prompt.app_name,"Changed after post");pump(1);
    assert(g_dialog && trusted_overlay && !g_dialog->busy && !strcmp(g_dialog->prompt.app_name,"Quake"));
    assert(!strcmp(lv_label_get_text(g_dialog->details),"Need 2.86 MiB / Free 3.81 MiB"));
    assert(lv_obj_get_style_border_width(g_dialog->panel,0)==1 && lv_obj_get_style_shadow_width(g_dialog->panel,0)==0);
    assert(lv_obj_get_style_radius(g_dialog->panel,0)==pxa_esp_dialog_px(4)*3);
    assert(!lv_obj_has_state(g_dialog->choice,LV_STATE_DISABLED) && !lv_obj_is_clickable(g_dialog->choice));
    assert(lv_color_eq(lv_obj_get_style_bg_color(g_dialog->choice,0),color(PXA_UI_THEME_SURFACE_CONTAINER_HIGHEST)));
    lv_obj_t *cancel=lv_obj_get_child(lv_obj_get_parent(g_dialog->install),0);
    lv_obj_send_event(cancel,LV_EVENT_CLICKED,NULL);
    assert(!g_dialog && !trusted_overlay && decisions==1 && !accepted && decision_id==41);
    assert(timer_count()==baseline);
    for(uint32_t id=42;id<46;++id) {
        assert(pxa_esp_ui_shell_post_move_progress(id,"Quake"));pump(1);
        assert(g_dialog && g_dialog->busy && trusted_overlay && g_dialog->progress_timer);
        assert(timer_count()==baseline+1);
        const int32_t angle=lv_arc_get_rotation(g_dialog->progress);
        pump(100);assert(lv_arc_get_rotation(g_dialog->progress)==angle);
        pump(25);assert(lv_arc_get_rotation(g_dialog->progress)==(angle+45)%360);
        uint32_t escape=LV_KEY_ESC;lv_obj_send_event(g_dialog->mask,LV_EVENT_KEY,&escape);
        assert(g_dialog && g_dialog->busy && decisions==1);
        /* A stale response for another prompt cannot remove current progress. */
        pxa_esp_ui_shell_dismiss_install_prompt(id-1);pump(1);assert(g_dialog && g_dialog->busy);
        pxa_esp_ui_shell_post_move_result(id,PXA_STATUS_OK);pump(1);
        assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
        pump(200);assert(!g_dialog); /* The closed dialog's timer cannot fire. */
    }
    assert(toasts==4);
    /* Cancellation may reach LVGL before the earlier show timer. */
    prompt.prompt_id=50;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));
    pxa_esp_ui_shell_dismiss_install_prompt(50);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    /* A small install can finish before its busy timer is delivered. */
    prompt.prompt_id=51;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));
    assert(pxa_esp_ui_shell_post_file_install_progress(51,"Tiny app",0));
    pxa_esp_ui_shell_post_file_install_result(51,PXA_STATUS_OK);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    /* An older result cannot close a newer confirmation. */
    pxa_esp_ui_shell_post_file_install_result(51,PXA_STATUS_OK);
    prompt.prompt_id=52;assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(g_dialog && !g_dialog->busy && g_dialog->prompt.prompt_id==52);
    pxa_esp_ui_shell_dismiss_install_prompt(52);pump(1);assert(!g_dialog);
    prompt.prompt_id=53;prompt.move=0;prompt.resources_only=1;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(g_dialog && g_dialog->prompt.resources_only);
    lv_obj_send_event(g_dialog->install,LV_EVENT_CLICKED,NULL);
    assert(!g_dialog && decisions==2 && accepted && decision_id==53);
    assert(pxa_esp_ui_shell_post_file_install_progress(53,"Game resources",1));pump(1);
    assert(g_dialog && g_dialog->busy && !g_dialog->prompt.move && g_dialog->prompt.resources_only);
    pxa_esp_ui_shell_post_file_install_result(53,PXA_STATUS_OK);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline && toasts==7);
    /* Slow preflight is visible; a delayed checking callback cannot replace
     * the later confirmation or actual installation for the same request. */
    assert(pxa_esp_ui_shell_post_file_prepare_progress(54,"offline.pxa"));pump(1);
    assert(g_dialog && g_dialog->busy && trusted_overlay && timer_count()==baseline+1);
    prompt.prompt_id=54;assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(g_dialog && !g_dialog->busy && timer_count()==baseline);
    assert(pxa_esp_ui_shell_post_file_prepare_progress(54,"offline.pxa"));
    assert(pxa_esp_ui_shell_post_file_install_progress(54,"Game resources",1));pump(1);
    assert(g_dialog && g_dialog->busy && g_dialog->prompt.resources_only && timer_count()==baseline+1);
    pxa_esp_ui_shell_post_file_install_result(54,PXA_STATUS_OK);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline && toasts==8);
    /* Real popup layout and palette states at compact, portrait and dense
     * display sizes. Selection stays in the trusted root and on screen. */
    const int32_t sizes[][3]={{320,240,160},{240,320,160},{480,320,240}};
    lv_style_t board_focus;lv_style_init(&board_focus);
    lv_style_set_outline_color(&board_focus,lv_color_hex(0xffb74d));lv_style_set_outline_width(&board_focus,2);
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        light_palette=i!=0;lv_display_set_resolution(display,sizes[i][0],sizes[i][1]);lv_display_set_dpi(display,sizes[i][2]);
        prompt.prompt_id=60+i;prompt.count=8;prompt.selected=0;prompt.resources_only=0;
        for(unsigned j=0;j<8;++j)strcpy(prompt.volumes[j].name,"Long storage volume");
        assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);lv_obj_update_layout(g_dialog->mask);
        assert(lv_obj_get_width(g_dialog->panel)<sizes[i][0] && lv_obj_get_height(g_dialog->panel)<sizes[i][1]);
        assert(lv_color_eq(lv_obj_get_style_bg_color(g_dialog->panel,0),color(PXA_UI_THEME_SURFACE_CONTAINER_HIGH)));
        lv_obj_t *primary=g_dialog->install,*secondary=lv_obj_get_child(lv_obj_get_parent(primary),0);
        assert(lv_obj_get_style_border_width(primary,0)==0 && lv_obj_get_style_border_width(secondary,0)==1);
        assert(lv_obj_get_style_text_font(lv_obj_get_child(primary,0),0)==pxa_esp_host_ui_label_font());
        lv_obj_add_style(primary,&board_focus,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);
        lv_obj_add_state(primary,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);pump(300);
        assert(lv_color_eq(lv_obj_get_style_outline_color(primary,0),color(PXA_UI_THEME_ON_PRIMARY)));
        lv_obj_add_state(primary,LV_STATE_PRESSED);pump(1);
        assert(lv_color_eq(lv_obj_get_style_outline_color(primary,0),color(PXA_UI_THEME_ON_PRIMARY_CONTAINER)));
        lv_obj_remove_state(primary,LV_STATE_PRESSED);
        lv_dropdown_open(g_dialog->choice);pump(1);lv_obj_t *list=lv_dropdown_get_list(g_dialog->choice);
        lv_obj_add_state(g_dialog->choice,LV_STATE_FOCUSED);pump(300);
        assert(lv_color_eq(lv_obj_get_style_border_color(g_dialog->choice,0),color(PXA_UI_THEME_PRIMARY)));
        lv_obj_add_style(g_dialog->choice,&board_focus,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);
        lv_obj_add_state(g_dialog->choice,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY);pump(300);
        assert(lv_color_eq(lv_obj_get_style_outline_color(g_dialog->choice,0),color(PXA_UI_THEME_PRIMARY)));
        assert(lv_obj_get_parent(list)==g_dialog->mask);lv_area_t area;lv_obj_get_coords(list,&area);
        assert(area.x1>=0 && area.y1>=0 && area.x2<sizes[i][0] && area.y2<sizes[i][1]);
        assert(lv_color_eq(lv_obj_get_style_bg_color(list,LV_PART_SELECTED),color(PXA_UI_THEME_PRIMARY_CONTAINER)));
        pxa_esp_ui_shell_dismiss_install_prompt(prompt.prompt_id);pump(1);
        assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    }
    lv_style_reset(&board_focus);
    /* Shell metrics are physical pixels; do not scale them a second time.
     * Square themes must not be mistaken for an uninitialized theme. */
    theme_spacing=5;theme_radius=0;
    prompt.prompt_id=79;prompt.count=1;prompt.move=0;prompt.media_count=1;
    strcpy(prompt.media_volumes[0].name,"SD card");prompt.media_fit[0]=0;
    prompt.same_media[0]=UINT8_MAX;prompt.media_reuse[0]=1;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(lv_obj_get_style_radius(g_dialog->panel,0)==0);
    assert(lv_obj_get_style_pad_left(g_dialog->panel,0)==15);
    assert(lv_obj_has_state(g_dialog->install,LV_STATE_DISABLED));
    lv_obj_add_state(g_dialog->install,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY|LV_STATE_PRESSED);pump(300);
    assert(lv_obj_get_style_outline_width(g_dialog->install,0)==0);
    assert(lv_color_eq(lv_obj_get_style_text_color(g_dialog->install,0),color(PXA_UI_THEME_MUTED)));
    lv_obj_remove_state(g_dialog->install,LV_STATE_FOCUSED|LV_STATE_FOCUS_KEY|LV_STATE_PRESSED);
    assert(lv_color_eq(lv_obj_get_style_text_color(g_dialog->details,0),color(PXA_UI_THEME_DANGER)));
    prompt.media_fit[0]=1;g_dialog->prompt.media_fit[0]=1;update_details();
    assert(!lv_obj_has_state(g_dialog->install,LV_STATE_DISABLED));
    /* A newly enabled action must use its semantic color immediately. An
     * inherited LVGL transition used to retain the disabled background. */
    assert(lv_color_eq(lv_obj_get_style_bg_color(g_dialog->install,0),color(PXA_UI_THEME_PRIMARY)));
    assert(lv_color_eq(lv_obj_get_style_text_color(g_dialog->install,0),color(PXA_UI_THEME_ON_PRIMARY)));
    assert(lv_color_eq(lv_obj_get_style_text_color(g_dialog->details,0),color(PXA_UI_THEME_MUTED)));
    pxa_esp_ui_shell_dismiss_install_prompt(79);pump(1);
    theme_spacing=0;theme_radius=0;prompt.media_count=0;
    prompt.prompt_id=80;prompt.count=1;prompt.move=1;prompt.resources_only=1;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(g_dialog && g_dialog->prompt.move && g_dialog->prompt.resources_only);
    assert(pxa_esp_ui_shell_post_move_progress_kind(80,"Game resources",1));pump(1);
    assert(g_dialog && g_dialog->busy && g_dialog->prompt.move && g_dialog->prompt.resources_only);
    pxa_esp_ui_shell_post_move_result(80,PXA_STATUS_OK);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    prompt.prompt_id=81;prompt.count=2;prompt.move=0;prompt.resources_only=0;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    for(uint32_t i=0;i<lv_obj_get_event_count(g_dialog->mask);++i)
        assert((lv_obj_get_event_dsc(g_dialog->mask,i)->filter&~LV_EVENT_PREPROCESS)!=LV_EVENT_KEY);
    lv_dropdown_open(g_dialog->choice);pump(1);
    uint32_t escape=LV_KEY_ESC;lv_obj_send_event(g_dialog->choice,LV_EVENT_KEY,&escape);pump(1);
    assert(g_dialog && !lv_dropdown_is_open(g_dialog->choice));
    cancel=lv_obj_get_child(lv_obj_get_parent(g_dialog->install),0);
    lv_obj_send_event(cancel,LV_EVENT_KEY,&escape);pump(1);
    assert(!g_dialog && !trusted_overlay && !accepted && decision_id==81 && timer_count()==baseline);
    /* A same-volume storage upgrade retains its separate media location and
     * presents a readable, noninteractive single location. */
    prompt.prompt_id=82;prompt.move=1;prompt.legacy_upgrade=1;prompt.count=1;
    strcpy(prompt.app_name,"Quake");
    strcpy(prompt.retained_media_volume,"SD card");
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(label_with_text(g_dialog->panel,"Quake · Storage upgrade"));
    assert(label_with_text(g_dialog->install,"Upgrade"));
    assert(label_with_text(g_dialog->panel,"Storage"));
    lv_obj_t *media_caption=label_with_text(g_dialog->panel,"Resources stay on SD card");
    assert(media_caption && lv_label_get_long_mode(media_caption)==LV_LABEL_LONG_WRAP);
    assert(lv_color_eq(lv_obj_get_style_text_color(media_caption,0),color(PXA_UI_THEME_MUTED)));
    assert(!lv_obj_is_clickable(g_dialog->choice) && !lv_obj_has_state(g_dialog->choice,LV_STATE_DISABLED));
    pxa_esp_ui_shell_dismiss_install_prompt(82);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    /* Capacity labels share exact binary units for installs and moves.
     * Same-volume resources use the planner's peak, never two full peaks. */
    prompt=(pxa_esp_ui_install_prompt_t){.prompt_id=83,.count=1,.media_count=1};
    strcpy(prompt.app_name,"Game");strcpy(prompt.volumes[0].name,"Flash");
    strcpy(prompt.media_volumes[0].name,"SD");
    prompt.volumes[0].required_bytes=900000;prompt.volumes[0].free_bytes=1245184;prompt.volumes[0].free_known=1;
    prompt.media_volumes[0].required_bytes=23680;prompt.media_volumes[0].free_bytes=10000000;prompt.media_volumes[0].free_known=1;
    prompt.media_fit[0]=1;prompt.same_media[0]=0;prompt.combined_bytes[0][0]=1100000;
    assert(pxa_esp_ui_shell_post_install_prompt(&prompt));pump(1);
    assert(!strcmp(lv_label_get_text(g_dialog->details),"Need 1.05 MiB / Free 1.19 MiB"));
    assert(!lv_obj_has_state(g_dialog->install,LV_STATE_DISABLED));
    g_dialog->prompt.same_media[0]=UINT8_MAX;update_details();
    assert(!strcmp(lv_label_get_text(g_dialog->details),"App: Need 878.91 KiB / Free 1.19 MiB\nResources: Need 23.13 KiB / Free 9.54 MiB"));
    g_dialog->prompt.media_reuse[0]=1;update_details();
    assert(strstr(lv_label_get_text(g_dialog->details),"Resources: reuse existing files"));
    chinese_locale=true;g_dialog->prompt.media_count=0;update_details();
    assert(!strcmp(lv_label_get_text(g_dialog->details),"需 878.91 KiB / 可用 1.19 MiB"));
    g_dialog->prompt.volumes[0].required_bytes=1048577;g_dialog->prompt.volumes[0].free_bytes=1048576;update_details();
    assert(!strcmp(lv_label_get_text(g_dialog->details),"需 1048577 B / 可用 1048576 B"));
    assert(lv_obj_has_state(g_dialog->install,LV_STATE_DISABLED));
    g_dialog->prompt.volumes[0].free_known=0;update_details();
    assert(strstr(lv_label_get_text(g_dialog->details),"可用 未知"));
    pxa_esp_ui_shell_dismiss_install_prompt(83);pump(1);
    assert(!g_dialog && !trusted_overlay && timer_count()==baseline);
    lv_deinit();
    puts("Native installer LVGL: copied prompts, cancel, trusted busy overlay, bounded 8 Hz progress, stale IDs and timer cleanup passed");
    return 0;
}
