#include "pxa_esp_ui_shell.h"

#if defined(ESP_PLATFORM)

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "lvgl.h"

#include "pxa_esp_host.h"
#include "pxa_esp_dialog_layout.h"

static lv_obj_t *g_result_dialog;
static uint32_t g_result_prompt_id;

static lv_color_t theme_color(uint8_t index) {
    return lv_color_hex(pxa_esp_host_ui_color(index) >> 8);
}

static const lv_font_t *body_font(void) {
    const lv_font_t *font = pxa_esp_host_ui_body_font();
    return font != NULL ? font : pxa_esp_ui_shell_text_font();
}

static const lv_font_t *title_font(void) {
    const lv_font_t *font = pxa_esp_host_ui_title_font();
    return font != NULL ? font : pxa_esp_ui_shell_title_font();
}

static void close_result(void) {
    if (g_result_dialog == NULL) return;
    lv_obj_add_flag(g_result_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(g_result_dialog);
    g_result_dialog = NULL;
    g_result_prompt_id = 0;
}

static void result_response(lv_event_t *event) {
    uint32_t prompt_id = g_result_prompt_id;
    bool open = (uintptr_t)lv_event_get_user_data(event) != 0;
    close_result();
    (void)pxa_esp_host_respond_store_result(prompt_id, open);
}

static void show_result(void *context) {
    pxa_esp_ui_store_result_t *result = context;
    bool chinese = pxa_esp_host_locale_is_chinese();
    const char *heading = result->is_uninstall ?
        (chinese ? "卸载完成" : "Uninstalled") :
        (chinese ? "安装成功" : "Installed");
    const char *message = result->is_uninstall ?
        (chinese ? "应用已从设备移除" : "The app was removed from this device") :
        (chinese ? "应用已就绪，是否打开？" : "The app is ready. Open it now?");
    pxa_esp_dialog_layout_t layout = pxa_esp_dialog_measure(
        lv_display_get_horizontal_resolution(lv_display_get_default()),
        lv_display_get_vertical_resolution(lv_display_get_default()),
        heading, result->app_name, message, title_font(), body_font(), 0, 6);
    lv_obj_t *panel;
    lv_obj_t *title;
    lv_obj_t *content;
    lv_obj_t *name;
    lv_obj_t *body;
    lv_obj_t *confirm;
    lv_obj_t *label;
    lv_color_t primary;
    close_result();
    g_result_prompt_id = result->prompt_id;
    g_result_dialog = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_result_dialog);
    lv_obj_set_size(g_result_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_result_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_result_dialog, LV_OPA_60, 0);
    lv_obj_add_flag(g_result_dialog, LV_OBJ_FLAG_CLICKABLE);

    panel = lv_obj_create(g_result_dialog);
    lv_obj_set_size(panel, layout.width, layout.height);
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_bg_color(panel, theme_color(1), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, theme_color(6), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    primary = theme_color(2);

    title = lv_label_create(panel);
    lv_label_set_text(title, heading);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, title_font(), 0);
    lv_obj_set_style_text_color(title, theme_color(4), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    content = lv_obj_create(panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), layout.content_height);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, layout.content_top);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);

    name = lv_label_create(content);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name, result->app_name);
    lv_obj_set_height(name, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(name, body_font(), 0);
    lv_obj_set_style_text_color(name, theme_color(4), 0);

    body = lv_label_create(content);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body, LV_SIZE_CONTENT);
    lv_label_set_text(body, message);
    lv_obj_set_style_text_font(body, body_font(), 0);
    lv_obj_set_style_text_color(body, theme_color(5), 0);

    confirm = lv_button_create(panel);
    lv_obj_set_size(confirm, layout.button_width, 36);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT,
                 result->is_uninstall ? 0 : -layout.button_width - 8, 0);
    lv_obj_set_style_bg_opa(confirm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(confirm, primary, 0);
    lv_obj_set_style_border_width(confirm, 1, 0);
    lv_obj_add_event_cb(confirm, result_response, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(confirm);
    lv_label_set_text(label, chinese ? "确认" : "Confirm");
    lv_obj_set_style_text_color(label, primary, 0);
    lv_obj_set_style_text_font(label, body_font(), 0);
    lv_obj_center(label);

    if (!result->is_uninstall) {
        lv_obj_t *open = lv_button_create(panel);
        lv_obj_set_size(open, layout.button_width, 36);
        lv_obj_align(open, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(open, primary, 0);
        lv_obj_add_event_cb(open, result_response, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)1);
        label = lv_label_create(open);
        lv_label_set_text(label, chinese ? "打开" : "Open");
        lv_obj_set_style_text_color(label, theme_color(3), 0);
        lv_obj_set_style_text_font(label, body_font(), 0);
        lv_obj_center(label);
    }
    free(result);
}

static void dismiss_result(void *context) {
    uint32_t prompt_id = (uint32_t)(uintptr_t)context;
    if (g_result_prompt_id == prompt_id) close_result();
}

int pxa_esp_ui_shell_post_store_result(const pxa_esp_ui_store_result_t *result) {
    pxa_esp_ui_store_result_t *copy;
    lv_result_t status;
    if (result == NULL) return 0;
    copy = malloc(sizeof(*copy));
    if (copy == NULL) return 0;
    *copy = *result;
    lv_lock();
    status = lv_async_call(show_result, copy);
    lv_unlock();
    if (status == LV_RESULT_OK) return 1;
    free(copy);
    return 0;
}

void pxa_esp_ui_shell_dismiss_store_result(uint32_t prompt_id) {
    lv_lock();
    (void)lv_async_call(dismiss_result, (void *)(uintptr_t)prompt_id);
    lv_unlock();
}

#endif
