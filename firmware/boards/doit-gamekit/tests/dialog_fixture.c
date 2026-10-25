/* Compile the production permission / ANR dialogs, not an approximate mask. */
#include "lvgl.h"
#define ESP_PLATFORM 1
#include "../../../components/pxa/src/ui/pxa_esp_standalone.c"
#include <assert.h>

unsigned dialog_decisions;
uint32_t dialog_id;
bool dialog_accepted;
static lv_obj_t *dialog_root;
void pxa_esp_host_ui_metrics(uint16_t *spacing, uint16_t *radius) {
    *spacing = 4; *radius = 4;
}
bool pxa_esp_host_locale_is_chinese(void) { return false; }
uint32_t pxa_esp_host_ui_color(uint8_t role) { return 0x203040ffu + role * 0x01010100u; }
const lv_font_t *pxa_esp_host_ui_body_font(void) { return LV_FONT_DEFAULT; }
const lv_font_t *pxa_esp_host_ui_label_font(void) { return LV_FONT_DEFAULT; }
const lv_font_t *pxa_esp_host_ui_title_font(void) { return LV_FONT_DEFAULT; }
void pxa_esp_system_overlay_add(lv_obj_t *object) { dialog_root = object; }
void pxa_esp_system_overlay_remove(lv_obj_t *object) {
    if (dialog_root == object) dialog_root = NULL;
}
void pxa_esp_system_overlay_refresh(void) {}
bool pxa_esp_host_respond_permission(uint32_t id, bool accepted) {
    ++dialog_decisions; dialog_id = id; dialog_accepted = accepted; return true;
}
bool pxa_esp_host_respond_unresponsive(uint32_t id, bool wait) {
    return pxa_esp_host_respond_permission(id, wait);
}
lv_obj_t *test_dialog_show(bool unresponsive, uint32_t id) {
    if (unresponsive) {
        pxa_esp_ui_unresponsive_prompt_t prompt = {.prompt_id = id};
        strcpy(prompt.app_name, "Test game");
        assert(pxa_esp_ui_shell_post_unresponsive_prompt(&prompt));
    } else {
        pxa_esp_ui_permission_prompt_t prompt = {.prompt_id = id};
        strcpy(prompt.app_name, "Test game");
        strcpy(prompt.permission_name, "Audio playback");
        assert(pxa_esp_ui_shell_post_permission_prompt(&prompt));
    }
    lv_tick_inc(1); lv_timer_handler();
    assert(dialog_root);
    return dialog_root;
}

void test_dialog_recover(uint32_t id) {
    pxa_esp_ui_shell_dismiss_unresponsive_prompt(id);
    lv_tick_inc(1); lv_timer_handler();
}
