#ifndef PXA_ESP_DIALOG_STYLE_H
#define PXA_ESP_DIALOG_STYLE_H

#include "pxa_esp_dialog_layout.h"
#include "pxa_esp_host.h"
#include "pxa/ui.h"
#include "pxa/lvgl/pxa_lvgl_dialog_style.h"

/* Native dialogs share the shell palette and existing dialog density scale.
 * Override interactive states too, rather than mixing in LVGL theme colors. */
static inline lv_color_t pxa_esp_dialog_color(uint8_t role) {
    return lv_color_hex(pxa_esp_host_ui_color(role)>>8);
}
static inline int32_t pxa_esp_dialog_px(int32_t value) {
    return pxa_esp_dialog_scale(lv_display_get_dpi(lv_display_get_default()),value);
}
static inline int32_t pxa_esp_dialog_spacing(unsigned units) {
    uint16_t spacing,radius;pxa_esp_host_ui_metrics(&spacing,&radius);
    return (spacing?spacing:pxa_esp_dialog_px(4))*(int32_t)units;
}
static inline int32_t pxa_esp_dialog_radius(unsigned units) {
    uint16_t spacing,radius;pxa_esp_host_ui_metrics(&spacing,&radius);
    return (spacing?radius:pxa_esp_dialog_px(4))*(int32_t)units;
}
static inline void pxa_esp_dialog_style_mask(lv_obj_t *mask) {
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask,LV_PCT(100),LV_PCT(100));
    lv_obj_set_style_bg_color(mask,lv_color_black(),0);
    lv_obj_set_style_bg_opa(mask,LV_OPA_60,0);
    lv_obj_set_clickable(mask,true);
    lv_obj_set_click_focusable(mask,false);
    lv_obj_set_scrollable(mask,false);
}
static inline void pxa_esp_dialog_style_scrollbar(lv_obj_t *object) {
    lv_obj_set_style_width(object,pxa_esp_dialog_px(2),LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(object,pxa_esp_dialog_px(2),LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_top(object,pxa_esp_dialog_spacing(1),LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_bottom(object,pxa_esp_dialog_spacing(1),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(object,pxa_esp_dialog_color(PXA_UI_THEME_BORDER),LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(object,LV_OPA_60,LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(object,LV_RADIUS_CIRCLE,LV_PART_SCROLLBAR);
}
static inline pxa_lvgl_dialog_style_t pxa_esp_dialog_theme(void) {
    const lv_font_t *label = pxa_esp_host_ui_label_font();
    if(!label) label = pxa_esp_host_ui_body_font();
    return (pxa_lvgl_dialog_style_t){
        .surface = pxa_esp_dialog_color(PXA_UI_THEME_SURFACE),
        .container = pxa_esp_dialog_color(PXA_UI_THEME_SURFACE_CONTAINER),
        .panel = pxa_esp_dialog_color(PXA_UI_THEME_SURFACE_CONTAINER_HIGH),
        .text = pxa_esp_dialog_color(PXA_UI_THEME_TEXT),
        .muted = pxa_esp_dialog_color(PXA_UI_THEME_MUTED),
        .outline = pxa_esp_dialog_color(PXA_UI_THEME_OUTLINE_VARIANT),
        .primary = pxa_esp_dialog_color(PXA_UI_THEME_PRIMARY),
        .on_primary = pxa_esp_dialog_color(PXA_UI_THEME_ON_PRIMARY),
        .primary_container = pxa_esp_dialog_color(PXA_UI_THEME_PRIMARY_CONTAINER),
        .on_primary_container = pxa_esp_dialog_color(PXA_UI_THEME_ON_PRIMARY_CONTAINER),
        .error_container = pxa_esp_dialog_color(PXA_UI_THEME_ERROR_CONTAINER),
        .on_error_container = pxa_esp_dialog_color(PXA_UI_THEME_ON_ERROR_CONTAINER),
        .spacing = pxa_esp_dialog_spacing(1),
        .radius = pxa_esp_dialog_radius(1),
        .label_font = label,
    };
}
static inline void pxa_esp_dialog_style_panel(lv_obj_t *panel) {
    const pxa_lvgl_dialog_style_t theme = pxa_esp_dialog_theme();
    lv_obj_set_scrollable(panel,false);
    pxa_lvgl_dialog_panel_style(panel,&theme);
}
static inline void pxa_esp_dialog_style_focus(lv_obj_t *object) {
    pxa_lvgl_dialog_focus_style(object,pxa_esp_dialog_color(PXA_UI_THEME_PRIMARY));
}
static inline void pxa_esp_dialog_style_action(lv_obj_t *button,pxa_lvgl_action_t action) {
    const pxa_lvgl_dialog_style_t theme = pxa_esp_dialog_theme();
    pxa_lvgl_dialog_action_style(button,&theme,action);
}
static inline void pxa_esp_dialog_style_button(lv_obj_t *button,bool primary) {
    pxa_esp_dialog_style_action(button,primary?PXA_LVGL_ACTION_PRIMARY:PXA_LVGL_ACTION_SECONDARY);
}
static inline void pxa_esp_dialog_style_dropdown(lv_obj_t *dropdown) {
    lv_obj_remove_theme(dropdown,LV_PART_MAIN|LV_STATE_ANY);
    lv_obj_set_style_radius(dropdown,pxa_esp_dialog_radius(2),0);
    lv_obj_set_style_bg_color(dropdown,pxa_esp_dialog_color(PXA_UI_THEME_SURFACE_CONTAINER_HIGHEST),0);
    lv_obj_set_style_bg_opa(dropdown,LV_OPA_COVER,0);
    lv_obj_set_style_text_color(dropdown,pxa_esp_dialog_color(PXA_UI_THEME_TEXT),0);
    lv_obj_set_style_border_width(dropdown,1,0);
    lv_obj_set_style_border_color(dropdown,pxa_esp_dialog_color(PXA_UI_THEME_OUTLINE_VARIANT),0);
    lv_obj_set_style_shadow_width(dropdown,0,0);
    lv_obj_set_style_pad_hor(dropdown,pxa_esp_dialog_px(10),0);
    lv_obj_set_style_pad_ver(dropdown,pxa_esp_dialog_px(6),0);
    lv_obj_set_style_bg_color(dropdown,pxa_esp_dialog_color(PXA_UI_THEME_PRIMARY_CONTAINER),LV_STATE_PRESSED);
    lv_obj_set_style_text_color(dropdown,pxa_esp_dialog_color(PXA_UI_THEME_ON_PRIMARY_CONTAINER),LV_STATE_PRESSED);
    pxa_esp_dialog_style_focus(dropdown);
    lv_obj_t *list=lv_dropdown_get_list(dropdown);
    lv_obj_set_style_bg_color(list,pxa_esp_dialog_color(PXA_UI_THEME_SURFACE),0);
    lv_obj_set_style_text_color(list,pxa_esp_dialog_color(PXA_UI_THEME_TEXT),0);
    lv_obj_set_style_border_width(list,1,0);
    lv_obj_set_style_border_color(list,pxa_esp_dialog_color(PXA_UI_THEME_OUTLINE_VARIANT),0);
    lv_obj_set_style_radius(list,pxa_esp_dialog_radius(2),0);
    lv_obj_set_style_shadow_width(list,0,0);
    lv_obj_set_style_pad_all(list,pxa_esp_dialog_spacing(1),0);
    lv_obj_set_style_text_line_space(list,pxa_esp_dialog_spacing(2),0);
    pxa_esp_dialog_style_scrollbar(list);
    const lv_state_t states[]={LV_STATE_DEFAULT,LV_STATE_CHECKED,LV_STATE_PRESSED};
    for(unsigned i=0;i<sizeof(states)/sizeof(states[0]);++i) {
        lv_style_selector_t selector=LV_PART_SELECTED|states[i];
        lv_obj_set_style_bg_opa(list,LV_OPA_COVER,selector);
        lv_obj_set_style_bg_color(list,pxa_esp_dialog_color(PXA_UI_THEME_PRIMARY_CONTAINER),selector);
        lv_obj_set_style_text_color(list,pxa_esp_dialog_color(PXA_UI_THEME_ON_PRIMARY_CONTAINER),selector);
    }
}
/* Navigation rows use the same interaction colors as actions, with a quiet
 * surface and no repeated outlines. Keypad focus remains visible. */
static inline void pxa_esp_dialog_style_list_item(lv_obj_t *item) {
    pxa_esp_dialog_style_button(item,false);
    lv_obj_set_style_border_width(item,0,0);
    lv_obj_set_style_border_width(item,0,LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(item,pxa_esp_dialog_color(PXA_UI_THEME_SURFACE),0);
    lv_obj_set_style_bg_opa(item,LV_OPA_COVER,0);
    lv_obj_set_style_pad_hor(item,pxa_esp_dialog_spacing(2),0);
    lv_obj_set_style_text_align(item,LV_TEXT_ALIGN_LEFT,0);
}
#endif
