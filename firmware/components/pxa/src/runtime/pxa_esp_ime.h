#ifndef PXA_ESP_IME_H
#define PXA_ESP_IME_H
#include "pxa/lvgl/pxa_lvgl_ime.h"
#include "pxa/wamr/pxa_wamr_engine.h"
#include "pxa_host_command.h"
#include "pxa/pxa_host.h"
/* Runtime owner only, except UI focus/selection/managed explicitly below.
 * Guest entry is never made under the LVGL lock. */
void pxa_esp_ime_initialize(uint64_t (*next_instance)(void),
    int (*post)(const pxa_esp_host_command_t *), void (*wake)(void));
bool pxa_esp_ime_needs_runtime(void);
bool pxa_esp_ime_active(void);
void pxa_esp_ime_failed(void);
void pxa_esp_ime_attach(pxa_runtime_t *runtime, pxa_wamr_engine_t *engine,
    const pxa_component_engine_t *operations, const pxa_package_host_profile_t *profile,
    uint64_t (*next_instance)(void), int (*post)(const pxa_esp_host_command_t *),
    void (*wake)(void));
void pxa_esp_ime_pump(void);
bool pxa_esp_ime_loading(void);
void pxa_esp_ime_drain(void);
void pxa_esp_ime_input(const pxa_host_ime_input_command_t *command);
pxa_status_t pxa_esp_ime_prepare(pxa_component_t component, uint64_t instance);
void pxa_esp_ime_stop(void);
/* After runtime deinit: service callbacks can no longer reference workspace. */
void pxa_esp_ime_release(void);
void pxa_esp_ime_suspend(void);
void pxa_esp_ime_editor_focus(void *context, void *textarea, bool focused);
void pxa_esp_ime_environment(const pxa_ui_environment_t *environment,
                            const pxa_lvgl_ui_theme_t *theme);
bool pxa_esp_ime_preferences(pxa_ime_preferences_t *preferences,bool save);
bool pxa_esp_ime_select(const char *identity);
bool pxa_esp_ime_selected(char *identity, size_t capacity);
bool pxa_esp_ime_managed(void *textarea);
void pxa_esp_ime_timing(pxa_host_input_method_timing_t *output);
/* Watchdog thread: only the active provider's call can be terminated. */
bool pxa_esp_ime_poll_deadline(uint64_t now_us);
#endif
