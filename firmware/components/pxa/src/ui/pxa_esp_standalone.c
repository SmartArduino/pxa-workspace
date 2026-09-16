#include "pxa_esp_ui_shell.h"

#if defined(ESP_PLATFORM)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "pxa_esp_host.h"

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

static lv_obj_t *g_permission_dialog;
static lv_obj_t *g_unresponsive_dialog;
static lv_obj_t *g_toast;
static lv_timer_t *g_toast_timer;
static uint32_t g_permission_prompt_id;
static uint32_t g_unresponsive_prompt_id;

extern const lv_font_t font_puhui_basic_14_1 __attribute__((weak));
extern const lv_font_t font_puhui_basic_20_4 __attribute__((weak));

static lv_result_t schedule_on_lvgl(lv_async_cb_t callback, void *context) {
    lv_result_t result;
    lv_lock();
    result = lv_async_call(callback, context);
    lv_unlock();
    return result;
}

static void close_dialog(lv_obj_t **dialog) {
    if (*dialog == NULL) return;
    lv_obj_delete_async(*dialog);
    *dialog = NULL;
}

static lv_obj_t *create_dialog(const char *title, const char *body,
                               const char *left_text, lv_event_cb_t left_cb,
                               const char *right_text, lv_event_cb_t right_cb) {
    lv_obj_t *mask = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_60, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(mask);
    lv_obj_set_size(panel, LV_PCT(86), 166);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x202124), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x5f6368), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);

    lv_obj_t *title_label = lv_label_create(panel);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xffffff), 0);
    if (&font_puhui_basic_20_4 != NULL) {
        lv_obj_set_style_text_font(title_label, &font_puhui_basic_20_4, 0);
    }

    lv_obj_t *body_label = lv_label_create(panel);
    lv_label_set_text(body_label, body);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(body_label, LV_PCT(100), 72);
    lv_obj_align(body_label, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_text_color(body_label, lv_color_hex(0xdadce0), 0);
    if (&font_puhui_basic_14_1 != NULL) {
        lv_obj_set_style_text_font(body_label, &font_puhui_basic_14_1, 0);
    }

    lv_obj_t *left = lv_button_create(panel);
    lv_obj_set_size(left, 92, 36);
    lv_obj_align(left, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(left, left_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)0);
    lv_obj_t *left_label = lv_label_create(left);
    lv_label_set_text(left_label, left_text);
    lv_obj_center(left_label);

    lv_obj_t *right = lv_button_create(panel);
    lv_obj_set_size(right, 92, 36);
    lv_obj_align(right, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(right, right_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)1);
    lv_obj_t *right_label = lv_label_create(right);
    lv_label_set_text(right_label, right_text);
    lv_obj_center(right_label);
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
    close_dialog(&g_permission_dialog);
    snprintf(body, sizeof(body), "%s\n%s\n%s", copy->prompt.app_name,
             copy->prompt.permission_name, copy->prompt.scope);
    g_permission_prompt_id = copy->prompt.prompt_id;
    g_permission_dialog = create_dialog("Permission request", body, "Deny",
                                        permission_response, "Allow",
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
    snprintf(body, sizeof(body), "%s is not responding.",
             copy->prompt.app_name);
    g_unresponsive_prompt_id = copy->prompt.prompt_id;
    g_unresponsive_dialog = create_dialog("App not responding", body, "Stop",
                                          unresponsive_response, "Wait",
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
    if (&font_puhui_basic_14_1 != NULL) {
        lv_obj_set_style_text_font(g_toast, &font_puhui_basic_14_1, 0);
    }
    g_toast_timer = lv_timer_create(toast_timeout, copy->duration_ms, NULL);
    if (g_toast_timer != NULL) lv_timer_set_repeat_count(g_toast_timer, 1);
    free(copy);
}

void pxa_esp_ui_shell_bind(void) {}

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

void pxa_esp_ui_shell_dismiss_app_launch(void) {}
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
