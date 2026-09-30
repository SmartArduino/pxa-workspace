#include "pxa_esp_ui_shell.h"

#if defined(ESP_PLATFORM)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "pxa_esp_host.h"
#include "pxa_esp_dialog_layout.h"
#include "pxa_esp_system_overlay.h"
#include "pxa_esp_package_icon.h"
#include "pxa_esp_package_store.h"
#include "pxa/ui.h"
#include "pxa/pxa_esp_surface.h"

#define PXA_STANDALONE_UI_TAG "PxaUiShell"

typedef struct {
    pxa_esp_ui_permission_prompt_t prompt;
} permission_prompt_copy_t;

typedef struct {
    pxa_esp_ui_unresponsive_prompt_t prompt;
} unresponsive_prompt_copy_t;

typedef struct {
    uint32_t duration_ms;
    char message[192];
} toast_copy_t;

typedef struct {
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
} launch_copy_t;

static lv_obj_t *g_permission_dialog;
static lv_obj_t *g_unresponsive_dialog;
static lv_obj_t *g_toast;
static lv_obj_t *g_launch_overlay;
static pxa_host_icon_t g_launch_icon;
static char g_launch_identity[PXA_ESP_PACKAGE_ID_BYTES];
static char g_requested_launch_identity[PXA_ESP_PACKAGE_ID_BYTES];
static lv_timer_t *g_launch_timeout;
static lv_timer_t *g_toast_timer;
static uint32_t g_permission_prompt_id;
static uint32_t g_unresponsive_prompt_id;

extern const lv_font_t font_puhui_basic_14_1 __attribute__((weak));
extern const lv_font_t font_puhui_basic_20_4 __attribute__((weak));

static const lv_font_t *system_body_font(void) {
    const lv_font_t *font = pxa_esp_host_ui_body_font();
    return font != NULL ? font : pxa_esp_ui_shell_text_font();
}

static const lv_font_t *system_title_font(void) {
    const lv_font_t *font = pxa_esp_host_ui_title_font();
    return font != NULL ? font : pxa_esp_ui_shell_title_font();
}

static lv_color_t system_color(uint8_t index) {
    return lv_color_hex(pxa_esp_host_ui_color(index) >> 8);
}

static lv_result_t schedule_on_lvgl(lv_async_cb_t callback, void *context) {
    lv_result_t result;
    lv_lock();
    result = lv_async_call(callback, context);
    lv_unlock();
    return result;
}

static void close_dialog(lv_obj_t **dialog) {
    if (*dialog == NULL) return;
    pxa_esp_system_overlay_remove(*dialog);
    lv_obj_add_flag(*dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(*dialog);
    *dialog = NULL;
}

static void refresh_scrolled_overlay(lv_event_t *event) {
    (void)event;
    pxa_esp_system_overlay_refresh();
}

static void close_launch(void) {
    if (g_launch_timeout != NULL) {
        lv_timer_delete(g_launch_timeout);
        g_launch_timeout = NULL;
    }
    if (g_launch_overlay != NULL) {
        pxa_esp_system_overlay_remove(g_launch_overlay);
        lv_obj_delete(g_launch_overlay);
        g_launch_overlay = NULL;
    }
    if (g_launch_icon.release != NULL)
        g_launch_icon.release(g_launch_icon.release_context);
    memset(&g_launch_icon, 0, sizeof(g_launch_icon));
    g_launch_identity[0] = '\0';
}

static void launch_timeout(lv_timer_t *timer) {
    (void)timer;
    g_launch_timeout = NULL;
    if (strcmp(g_requested_launch_identity, g_launch_identity) == 0)
        g_requested_launch_identity[0] = '\0';
    close_launch();
}

static void show_launch(void *context) {
    launch_copy_t *copy = (launch_copy_t *)context;
    pxa_esp_package_icon_source_t source;
    lv_obj_t *icon;
    int32_t icon_size;
    if (strcmp(g_requested_launch_identity, copy->identity) != 0) {
        free(copy);
        return;
    }
    if (strcmp(g_launch_identity, copy->identity) == 0) {
        free(copy);
        return;
    }
    close_launch();
    snprintf(g_launch_identity, sizeof(g_launch_identity), "%s", copy->identity);
    memset(&source, 0, sizeof(source));
    if (pxa_esp_package_store_icon_source(copy->identity,
            PXA_ESP_PACKAGE_ICON_INSTALLED, &source))
        g_launch_icon = pxa_esp_package_icon_load(source.root,
                                                  source.relative_path);

    g_launch_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_launch_overlay);
    lv_obj_set_size(g_launch_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_launch_overlay,
                              system_color(PXA_UI_THEME_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(g_launch_overlay, LV_OPA_COVER, 0);
    lv_obj_set_clickable(g_launch_overlay, true);
    lv_obj_move_to_index(g_launch_overlay, 0);

    icon_size = lv_display_get_horizontal_resolution(lv_display_get_default()) / 4;
    if (icon_size < 56) icon_size = 56;
    if (icon_size > 88) icon_size = 88;
    icon = lv_obj_create(g_launch_overlay);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, icon_size, icon_size);
    lv_obj_set_style_radius(icon, icon_size / 4, 0);
    lv_obj_set_style_bg_color(icon, system_color(PXA_UI_THEME_PRIMARY), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);
    if (g_launch_icon.image_dsc != NULL) {
        lv_obj_t *image = lv_image_create(icon);
        lv_obj_set_size(image, LV_PCT(100), LV_PCT(100));
        lv_image_set_src(image, g_launch_icon.image_dsc);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_center(image);
    } else {
        lv_obj_t *symbol = lv_label_create(icon);
        lv_label_set_text(symbol, LV_SYMBOL_LIST);
        lv_obj_set_style_text_color(symbol,
                                    system_color(PXA_UI_THEME_ON_PRIMARY), 0);
        lv_obj_set_style_text_font(symbol, pxa_esp_ui_shell_icon_font(), 0);
        lv_obj_center(symbol);
    }
    g_launch_timeout = lv_timer_create(launch_timeout, 30000, NULL);
    if (g_launch_timeout != NULL)
        lv_timer_set_repeat_count(g_launch_timeout, 1);
    pxa_esp_system_overlay_add(g_launch_overlay);
    free(copy);
}

static void dismiss_launch(void *context) {
    launch_copy_t *copy = (launch_copy_t *)context;
    if (strcmp(g_launch_identity, copy->identity) == 0 ||
        g_requested_launch_identity[0] == '\0') close_launch();
    free(copy);
}

static lv_obj_t *create_dialog(const char *title, const char *body,
                               const char *left_text, lv_event_cb_t left_cb,
                               const char *right_text, lv_event_cb_t right_cb) {
    pxa_esp_dialog_layout_t layout = pxa_esp_dialog_measure(
        lv_display_get_horizontal_resolution(lv_display_get_default()),
        lv_display_get_vertical_resolution(lv_display_get_default()),
        title, body, NULL, system_title_font(), system_body_font(), 3, 0);
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(mask);
    lv_obj_set_size(panel, layout.width, layout.height);
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel,
        system_color(PXA_UI_THEME_SURFACE_CONTAINER_HIGH), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel,
        system_color(PXA_UI_THEME_OUTLINE_VARIANT), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);

    lv_obj_t *title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_color(title_label, system_color(4), 0);
    lv_obj_set_style_text_font(title_label, system_title_font(), 0);

    lv_obj_t *content = lv_obj_create(panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), layout.content_height);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, layout.content_top);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *body_label = lv_label_create(content);
    lv_label_set_text(body_label, body);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body_label, LV_PCT(100));
    lv_obj_set_height(body_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(body_label, system_color(5), 0);
    lv_obj_set_style_text_font(body_label, system_body_font(), 0);
    lv_obj_set_style_text_line_space(body_label, 3, 0);
    lv_obj_add_event_cb(content, refresh_scrolled_overlay, LV_EVENT_SCROLL,
                        NULL);

    lv_obj_t *left = lv_button_create(panel);
    lv_obj_set_size(left, layout.button_width, 36);
    lv_obj_align(left, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(left, system_color(2), 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_add_event_cb(left, left_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)0);
    lv_obj_t *left_label = lv_label_create(left);
    lv_label_set_text(left_label, left_text);
    lv_obj_set_style_text_font(left_label, system_body_font(), 0);
    lv_obj_set_style_text_color(left_label, system_color(2), 0);
    lv_obj_center(left_label);

    lv_obj_t *right = lv_button_create(panel);
    lv_obj_set_size(right, layout.button_width, 36);
    lv_obj_align(right, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(right, system_color(2), 0);
    lv_obj_add_event_cb(right, right_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)1);
    lv_obj_t *right_label = lv_label_create(right);
    lv_label_set_text(right_label, right_text);
    lv_obj_set_style_text_font(right_label, system_body_font(), 0);
    lv_obj_set_style_text_color(right_label, system_color(3), 0);
    lv_obj_center(right_label);
    pxa_esp_system_overlay_add(mask);
    return mask;
}

static void permission_response(lv_event_t *event) {
    const bool granted = (uintptr_t)lv_event_get_user_data(event) != 0;
    const uint32_t prompt_id = g_permission_prompt_id;
    close_dialog(&g_permission_dialog);
    g_permission_prompt_id = 0;
    (void)pxa_esp_host_respond_permission(prompt_id, granted);
}

static void show_permission(void *context) {
    permission_prompt_copy_t *copy = (permission_prompt_copy_t *)context;
    char body[300];
    const bool chinese = pxa_esp_host_locale_is_chinese();
    close_dialog(&g_permission_dialog);
    snprintf(body, sizeof(body), "%s\n%s\n%s", copy->prompt.app_name,
             copy->prompt.permission_name, copy->prompt.scope);
    g_permission_prompt_id = copy->prompt.prompt_id;
    g_permission_dialog = create_dialog(copy->prompt.is_uninstall ?
                                            (chinese ? "卸载确认" : "Uninstall confirmation") :
                                        copy->prompt.is_install ?
                                            (chinese ? "安装确认" : "Install confirmation") :
                                            (chinese ? "权限请求" : "Permission request"),
                                        body, copy->prompt.is_install ||
                                              copy->prompt.is_uninstall ?
                                            (chinese ? "取消" : "Cancel") :
                                            (chinese ? "拒绝" : "Deny"),
                                        permission_response, copy->prompt.is_uninstall ?
                                            (chinese ? "卸载" : "Uninstall") :
                                        copy->prompt.is_install ?
                                            (chinese ? "安装" : "Install") :
                                            (chinese ? "允许" : "Allow"),
                                        permission_response);
    free(copy);
}

static void dismiss_permission(void *context) {
    const uint32_t prompt_id = (uint32_t)(uintptr_t)context;
    if (prompt_id == g_permission_prompt_id) {
        close_dialog(&g_permission_dialog);
        g_permission_prompt_id = 0;
    }
}

static void unresponsive_response(lv_event_t *event) {
    const bool wait = (uintptr_t)lv_event_get_user_data(event) != 0;
    const uint32_t prompt_id = g_unresponsive_prompt_id;
    close_dialog(&g_unresponsive_dialog);
    g_unresponsive_prompt_id = 0;
    (void)pxa_esp_host_respond_unresponsive(prompt_id, wait);
}

static void show_unresponsive(void *context) {
    unresponsive_prompt_copy_t *copy =
        (unresponsive_prompt_copy_t *)context;
    char body[160];
    close_dialog(&g_unresponsive_dialog);
    const bool chinese = pxa_esp_host_locale_is_chinese();
    snprintf(body, sizeof(body), chinese ? "%s 无响应。" : "%s is not responding.",
             copy->prompt.app_name);
    g_unresponsive_prompt_id = copy->prompt.prompt_id;
    g_unresponsive_dialog = create_dialog(chinese ? "应用无响应" : "App not responding",
                                          body, chinese ? "停止" : "Stop",
                                          unresponsive_response, chinese ? "等待" : "Wait",
                                          unresponsive_response);
    free(copy);
}

static void dismiss_unresponsive(void *context) {
    const uint32_t prompt_id = (uint32_t)(uintptr_t)context;
    if (prompt_id == g_unresponsive_prompt_id) {
        close_dialog(&g_unresponsive_dialog);
        g_unresponsive_prompt_id = 0;
    }
}

static void toast_timeout(lv_timer_t *timer) {
    (void)timer;
    close_dialog(&g_toast);
    g_toast_timer = NULL;
}

static void show_toast(void *context) {
    toast_copy_t *copy = (toast_copy_t *)context;
    if (g_toast_timer != NULL) {
        lv_timer_delete(g_toast_timer);
        g_toast_timer = NULL;
    }
    close_dialog(&g_toast);
    g_toast = lv_label_create(lv_layer_top());
    lv_label_set_text(g_toast, copy->message);
    lv_label_set_long_mode(g_toast, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_toast, LV_PCT(82));
    lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x303134), 0);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(g_toast, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_all(g_toast, 9, 0);
    lv_obj_set_style_radius(g_toast, 4, 0);
    lv_obj_set_style_text_font(g_toast, system_body_font(), 0);
    g_toast_timer = lv_timer_create(toast_timeout, copy->duration_ms, NULL);
    pxa_esp_system_overlay_add(g_toast);
    if (g_toast_timer != NULL)
        lv_timer_set_repeat_count(g_toast_timer, 1);
    else {
        close_dialog(&g_toast);
    }
    free(copy);
}

void pxa_esp_ui_shell_bind(void) {
    pxa_esp_system_overlay_bind();
}

int pxa_esp_ui_shell_post_permission_prompt(
    const pxa_esp_ui_permission_prompt_t *prompt) {
    permission_prompt_copy_t *copy;
    if (prompt == NULL) return 0;
    copy = (permission_prompt_copy_t *)malloc(sizeof(*copy));
    if (copy == NULL) return 0;
    copy->prompt = *prompt;
    if (schedule_on_lvgl(show_permission, copy) == LV_RESULT_OK) return 1;
    free(copy);
    return 0;
}

void pxa_esp_ui_shell_dismiss_permission_prompt(uint32_t prompt_id) {
    (void)schedule_on_lvgl(dismiss_permission,
                           (void *)(uintptr_t)prompt_id);
}

int pxa_esp_ui_shell_post_unresponsive_prompt(
    const pxa_esp_ui_unresponsive_prompt_t *prompt) {
    unresponsive_prompt_copy_t *copy;
    if (prompt == NULL) return 0;
    copy = (unresponsive_prompt_copy_t *)malloc(sizeof(*copy));
    if (copy == NULL) return 0;
    copy->prompt = *prompt;
    if (schedule_on_lvgl(show_unresponsive, copy) == LV_RESULT_OK) return 1;
    free(copy);
    return 0;
}

void pxa_esp_ui_shell_dismiss_unresponsive_prompt(uint32_t prompt_id) {
    (void)schedule_on_lvgl(dismiss_unresponsive,
                           (void *)(uintptr_t)prompt_id);
}

void pxa_esp_ui_shell_post_toast(const char *message, uint32_t duration_ms) {
    toast_copy_t *copy;
    ESP_LOGW(PXA_STANDALONE_UI_TAG, "PXA notice (%lu ms): %s",
             (unsigned long)duration_ms, message != NULL ? message : "");
    copy = (toast_copy_t *)calloc(1, sizeof(*copy));
    if (copy == NULL) return;
    copy->duration_ms = duration_ms == 0 ? 1 : duration_ms;
    snprintf(copy->message, sizeof(copy->message), "%s",
             message != NULL ? message : "");
    if (schedule_on_lvgl(show_toast, copy) != LV_RESULT_OK) free(copy);
}

void pxa_esp_ui_shell_post_app_launch(const char *identity) {
    launch_copy_t *copy;
    lv_result_t result;
    if (identity == NULL || identity[0] == '\0') return;
    copy = (launch_copy_t *)calloc(1, sizeof(*copy));
    if (copy == NULL) return;
    snprintf(copy->identity, sizeof(copy->identity), "%s", identity);
    lv_lock();
    result = lv_async_call(show_launch, copy);
    if (result == LV_RESULT_OK)
        snprintf(g_requested_launch_identity,
                 sizeof(g_requested_launch_identity), "%s", copy->identity);
    lv_unlock();
    if (result != LV_RESULT_OK) free(copy);
}

void pxa_esp_ui_shell_dismiss_app_launch(const char *identity) {
    launch_copy_t *copy;
    lv_result_t result;
    if (identity == NULL || identity[0] == '\0') return;
    copy = (launch_copy_t *)calloc(1, sizeof(*copy));
    if (copy == NULL) return;
    snprintf(copy->identity, sizeof(copy->identity), "%s", identity);
    lv_lock();
    if (strcmp(g_requested_launch_identity, copy->identity) == 0)
        g_requested_launch_identity[0] = '\0';
    result = lv_async_call(dismiss_launch, copy);
    lv_unlock();
    if (result != LV_RESULT_OK) free(copy);
}
void pxa_esp_ui_shell_refresh_apps(void) {}

const lv_font_t *pxa_esp_ui_shell_text_font(void) {
    return &font_puhui_basic_14_1 != NULL ? &font_puhui_basic_14_1
                                          : LV_FONT_DEFAULT;
}

const lv_font_t *pxa_esp_ui_shell_title_font(void) {
    return &font_puhui_basic_20_4 != NULL ? &font_puhui_basic_20_4
                                          : LV_FONT_DEFAULT;
}

const lv_font_t *pxa_esp_ui_shell_icon_font(void) {
    return LV_FONT_DEFAULT;
}

#endif
