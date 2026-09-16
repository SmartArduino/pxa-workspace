#ifndef PXA_ESP_UI_SHELL_H
#define PXA_ESP_UI_SHELL_H

#include <stdint.h>

typedef struct _lv_font_t lv_font_t;

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_ESP_UI_SHELL_APP_NAME_BYTES 64u
#define PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES 97u

typedef struct {
    uint32_t prompt_id;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
    char permission_name[PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES];
    char scope[PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES];
} pxa_esp_ui_permission_prompt_t;

typedef struct {
    uint32_t prompt_id;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
} pxa_esp_ui_unresponsive_prompt_t;

void pxa_esp_ui_shell_bind(void);

int pxa_esp_ui_shell_post_permission_prompt(
    const pxa_esp_ui_permission_prompt_t *prompt);
void pxa_esp_ui_shell_dismiss_permission_prompt(uint32_t prompt_id);

int pxa_esp_ui_shell_post_unresponsive_prompt(
    const pxa_esp_ui_unresponsive_prompt_t *prompt);
void pxa_esp_ui_shell_dismiss_unresponsive_prompt(uint32_t prompt_id);

void pxa_esp_ui_shell_post_toast(const char *message, uint32_t duration_ms);
void pxa_esp_ui_shell_dismiss_app_launch(void);
void pxa_esp_ui_shell_refresh_apps(void);
const lv_font_t *pxa_esp_ui_shell_text_font(void);
const lv_font_t *pxa_esp_ui_shell_title_font(void);
const lv_font_t *pxa_esp_ui_shell_icon_font(void);

#ifdef __cplusplus
}
#endif

#endif
