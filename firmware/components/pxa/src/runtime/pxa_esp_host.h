#ifndef PXA_ESP_HOST_H
#define PXA_ESP_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pxa/pxa_host.h"

typedef struct _lv_font_t lv_font_t;

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32 runtime host on the C stack: WAMR engine (C adapter), all C
 * services with ESP backends (LittleFS permission policy and private
 * data), esp_timer clock/watchdog and the FreeRTOS command queue.
 * Replaces the legacy C++ runtime host. */

bool pxa_esp_host_initialize(void);
bool pxa_esp_host_start_runtime(void);
void pxa_esp_host_sync_builtins(void);
bool pxa_esp_host_refresh_inbox(void);
bool pxa_esp_host_stage_package_file(const char *source_path);
bool pxa_esp_host_install_package_file(const char *source_path);
bool pxa_esp_host_launch(const char *identity);
bool pxa_esp_host_back(void);
bool pxa_esp_host_stop(const char *identity);
bool pxa_esp_host_is_active(const char *identity);
bool pxa_esp_host_captures_volume_keys(void);
bool pxa_esp_host_post_key(uint16_t key);
bool pxa_esp_host_set_color_scheme(pxa_host_color_scheme_t color_scheme);
bool pxa_esp_host_set_ui_palette(const uint32_t rgba[10]);
bool pxa_esp_host_set_locale(const char *locale, uint8_t text_direction);
bool pxa_esp_host_locale_is_chinese(void);
bool pxa_esp_host_set_window_insets(const pxa_window_insets_t *safe_insets,
                                    const pxa_window_insets_t *system_bar_insets);
bool pxa_esp_host_set_display_geometry(uint32_t shape, const uint16_t radii[4]);
bool pxa_esp_host_post_controller_state(uint8_t controller, bool connected,
                                        uint32_t buttons);
bool pxa_esp_host_active_identity(char *identity, size_t capacity);
void pxa_esp_host_set_runtime_event_callback(
    pxa_host_runtime_event_fn callback, void *context);
void pxa_esp_host_set_window_changed_callback(
    pxa_host_window_changed_fn callback, void *context);
void pxa_esp_host_set_system_request_callback(
    pxa_host_system_request_fn callback, void *context);
bool pxa_esp_host_complete_system_request(const char *app_id,
                                          uint32_t component,
                                          uint32_t request_id, int32_t status,
                                          const void *payload,
                                          size_t payload_size);
bool pxa_esp_host_post_system_event(const char *app_id, uint32_t component,
                                    uint16_t opcode, uint32_t request_id,
                                    const void *payload,
                                    size_t payload_size);
bool pxa_esp_host_post_app_system_event(const char *identity_key,
                                        uint16_t opcode, const void *payload,
                                        size_t payload_size);
size_t pxa_esp_host_package_count(void);
size_t pxa_esp_host_list_packages(pxa_host_package_info_t *packages,
                                  size_t capacity);
bool pxa_esp_host_resolve_package_metadata(
    const char *identity, const char *locale,
    pxa_host_package_metadata_t *metadata);
bool pxa_esp_host_deploy_package(const char *identity);
bool pxa_esp_host_deploy_package_detailed(
    const char *identity, pxa_host_package_deploy_result_t *result);
bool pxa_esp_host_manage_app(pxa_host_app_action_t action,
                             const char *identity);
size_t pxa_esp_host_list_app_permissions(const char *identity,
                                         pxa_host_app_permission_t *permissions,
                                         size_t capacity);
bool pxa_esp_host_set_permission(const char *identity,
                                 size_t permission_index, bool granted);
bool pxa_esp_host_respond_permission(uint32_t prompt_id, bool granted);
bool pxa_esp_host_respond_unresponsive(uint32_t prompt_id, bool wait);
void pxa_esp_host_set_audio_sink(pxa_host_audio_submit_fn submit,
                                 pxa_host_audio_flush_fn flush,
                                 void *context);
void pxa_esp_host_set_audio_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context);
const lv_font_t *pxa_esp_host_ui_body_font(void);
const lv_font_t *pxa_esp_host_ui_title_font(void);

#ifdef __cplusplus
}
#endif

#endif
