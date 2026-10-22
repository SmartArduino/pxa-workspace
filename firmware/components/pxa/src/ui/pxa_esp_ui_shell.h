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
    uint8_t is_install;
    uint8_t is_uninstall;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
    char permission_name[PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES];
    char scope[PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES];
} pxa_esp_ui_permission_prompt_t;

typedef struct {
    uint32_t prompt_id;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
    char purpose[257];
} pxa_esp_ui_directory_prompt_t;
int pxa_esp_ui_shell_post_directory_prompt(const pxa_esp_ui_directory_prompt_t *prompt);
void pxa_esp_ui_shell_dismiss_directory_prompt(uint32_t prompt_id);

typedef struct {
    uint64_t required_bytes,free_bytes;
    char name[64];
} pxa_esp_ui_install_volume_t;
typedef struct {
    uint32_t prompt_id;
    char app_name[64],version[32],publisher[17];
    uint8_t count,selected,download_only,permission_count;
    pxa_esp_ui_install_volume_t volumes[8];
    uint8_t media_count,media_selected;
    uint8_t media_fit[8],same_media[8],media_reuse[8];
    pxa_esp_ui_install_volume_t media_volumes[8];
    uint64_t combined_bytes[8][8];
    uint8_t move;
    uint8_t legacy_upgrade;
    uint8_t resources_only;
    char current_volume[64],retained_media_volume[64];
} pxa_esp_ui_install_prompt_t;
int pxa_esp_ui_shell_post_install_prompt(const pxa_esp_ui_install_prompt_t *);
void pxa_esp_ui_shell_dismiss_install_prompt(uint32_t prompt_id);
int pxa_esp_ui_shell_post_move_progress(uint32_t prompt_id,const char *app_name);
int pxa_esp_ui_shell_post_move_progress_kind(uint32_t,const char *,uint8_t resources_only);
void pxa_esp_ui_shell_post_move_result(uint32_t prompt_id,int32_t status);
int pxa_esp_ui_shell_post_file_install_progress(uint32_t,const char *,uint8_t resources_only);
int pxa_esp_ui_shell_post_file_prepare_progress(uint32_t,const char *file_name);
void pxa_esp_ui_shell_post_file_install_result(uint32_t,int32_t);

typedef struct {
    uint32_t prompt_id;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
} pxa_esp_ui_unresponsive_prompt_t;

typedef struct {
    uint32_t prompt_id;
    uint8_t is_uninstall;
    char app_name[PXA_ESP_UI_SHELL_APP_NAME_BYTES];
} pxa_esp_ui_store_result_t;

void pxa_esp_ui_shell_bind(void);

int pxa_esp_ui_shell_post_permission_prompt(
    const pxa_esp_ui_permission_prompt_t *prompt);
void pxa_esp_ui_shell_dismiss_permission_prompt(uint32_t prompt_id);

int pxa_esp_ui_shell_post_unresponsive_prompt(
    const pxa_esp_ui_unresponsive_prompt_t *prompt);
void pxa_esp_ui_shell_dismiss_unresponsive_prompt(uint32_t prompt_id);

int pxa_esp_ui_shell_post_store_result(const pxa_esp_ui_store_result_t *result);
void pxa_esp_ui_shell_dismiss_store_result(uint32_t prompt_id);

void pxa_esp_ui_shell_post_toast(const char *message, uint32_t duration_ms);
void pxa_esp_ui_shell_post_app_launch(const char *identity);
void pxa_esp_ui_shell_dismiss_app_launch(const char *identity);
void pxa_esp_ui_shell_refresh_apps(void);
const lv_font_t *pxa_esp_ui_shell_text_font(void);
const lv_font_t *pxa_esp_ui_shell_title_font(void);
const lv_font_t *pxa_esp_ui_shell_icon_font(void);

#ifdef __cplusplus
}
#endif

#endif
