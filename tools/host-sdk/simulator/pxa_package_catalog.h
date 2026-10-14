#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app_pages.h"
#include "pxa/window.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIMULATOR_PXA_PUBLISHER_ROOT_BYTES 32u
#define SIMULATOR_PXA_APP_ID_MAX 64u
#define SIMULATOR_PXA_APP_NAME_MAX 64u
#define SIMULATOR_PXA_APP_VERSION_MAX 64u
#define SIMULATOR_PXA_LOCALIZED_NAME_MAX 128u
#define SIMULATOR_PXA_APP_DESCRIPTION_MAX 512u
#define SIMULATOR_PXA_APP_ICON_PATH_MAX 256u

typedef struct {
    uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES];
    char app_id[SIMULATOR_PXA_APP_ID_MAX];
    char name[SIMULATOR_PXA_APP_NAME_MAX];
    char version[SIMULATOR_PXA_APP_VERSION_MAX];
    bool enabled;
    bool built_in;
} simulator_pxa_system_app_t;

typedef struct {
    char name[SIMULATOR_PXA_LOCALIZED_NAME_MAX + 1u];
    char description[SIMULATOR_PXA_APP_DESCRIPTION_MAX + 1u];
    char icon_path[SIMULATOR_PXA_APP_ICON_PATH_MAX + 1u];
} simulator_pxa_system_metadata_t;

typedef bool (*simulator_pxa_launch_request_fn)(
    void *context,
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id);
typedef bool (*simulator_pxa_close_request_fn)(void *context);
typedef void (*simulator_pxa_catalog_changed_fn)(void *context);
typedef void (*simulator_pxa_window_changed_fn)(
    void *context, const pxa_window_configuration_t *configuration);

size_t simulator_pxa_catalog_list(app_pages_app_t *apps, size_t capacity,
                                  void *user_data);
bool simulator_pxa_initialize(void);
bool simulator_pxa_game_render_self_test(void);
void simulator_pxa_shutdown(void);
/* Display metrics handed to PXA guests: logical size and safe-area insets
 * ordered top, right, bottom, left. Call before simulator_pxa_initialize. */
void simulator_pxa_set_display_metrics(uint32_t width, uint32_t height,
                                       const uint32_t safe_insets[4]);
bool simulator_pxa_set_locale(const char *locale, uint8_t text_direction);
bool simulator_pxa_start(const char *app_id);
void simulator_pxa_stop(void);
bool simulator_pxa_is_active(void);
/* 1: handled by the guest, 0: close the app, -1: delivery failed. */
int simulator_pxa_dispatch_back(void);
bool simulator_pxa_post_key(uint32_t key);
bool simulator_pxa_post_controller_state(uint8_t controller, bool connected,
                                         uint32_t buttons);
bool simulator_pxa_post_system_event(uint16_t opcode, const void *payload,
                                     size_t payload_size);
void simulator_pxa_pointer_probe_reset(void);
uint32_t simulator_pxa_pointer_probe_count(void);
void simulator_pxa_ui_event_probe_reset(void);
uint32_t simulator_pxa_ui_event_probe_count(void);
bool simulator_pxa_ipc_self_test(void);
bool simulator_pxa_arcade_self_test(void);
bool simulator_pxa_plane_shooter_self_test(void);
bool simulator_pxa_lab_self_test(void);
bool simulator_pxa_voxel_permission_self_test(void);
bool simulator_pxa_fs_self_test(void);
bool simulator_pxa_storage_self_test(void);
bool simulator_pxa_lease_self_test(void);
bool simulator_pxa_menu_scroll_self_test(void);
bool simulator_pxa_state_journal_self_test(void);
bool simulator_pxa_garden_freeze_test(void);
bool simulator_pxa_work_self_test(void);
bool simulator_pxa_net_self_test(void);
bool simulator_pxa_weather_self_test(void);
bool simulator_pxa_provider_self_test(void);
size_t simulator_pxa_managed_list(app_pages_managed_app_t *apps,
                                  size_t capacity);
bool simulator_pxa_manage(app_pages_app_action_t action, const char *app_id);
size_t simulator_pxa_system_list(simulator_pxa_system_app_t *apps,
                                 size_t capacity);
bool simulator_pxa_system_metadata(
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *locale,
    simulator_pxa_system_metadata_t *metadata);
app_pages_app_icon_t simulator_pxa_system_icon_path(
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *icon_path);
void simulator_pxa_set_system_callbacks(
    simulator_pxa_launch_request_fn launch_request,
    simulator_pxa_close_request_fn close_request,
    simulator_pxa_catalog_changed_fn catalog_changed,
    simulator_pxa_window_changed_fn window_changed, void *context);

#ifdef __cplusplus
}
#endif
