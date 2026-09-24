#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pxa/window.h"

#if defined(ESP_PLATFORM)

#ifdef __cplusplus
extern "C" {
#endif

/* Package metadata without LVGL resources. Suitable for non-UI clients. */
#define PXA_HOST_PACKAGE_ID_MAX 130
#define PXA_HOST_APP_ID_MAX 65
#define PXA_HOST_PACKAGE_NAME_MAX 64
#define PXA_HOST_PACKAGE_VERSION_MAX 32
#define PXA_HOST_PACKAGE_LOCALIZED_NAME_MAX 128
#define PXA_HOST_PACKAGE_DESCRIPTION_MAX 512
#define PXA_HOST_PACKAGE_ICON_PATH_MAX 256
#define PXA_HOST_PACKAGE_DEPLOY_STAGE_MAX 40
#define PXA_HOST_PUBLISHER_ROOT_BYTES 32
#define PXA_HOST_COMPONENT_ID_MAX 65
#define PXA_HOST_SYSTEM_SERVICE_ID UINT16_C(17)
#define PXA_HOST_SYSTEM_SERVICE_MAJOR UINT16_C(0)
#define PXA_HOST_SYSTEM_SERVICE_MINOR UINT16_C(1)
#define PXA_HOST_SYSTEM_INTENT_START UINT16_C(1)
#define PXA_HOST_SYSTEM_SERVICE_INVOKE UINT16_C(2)
#define PXA_HOST_SYSTEM_TOPIC_PUBLISH UINT16_C(3)
#define PXA_HOST_SYSTEM_TOPIC_SUBSCRIBE UINT16_C(4)
#define PXA_HOST_SYSTEM_TOPIC_UNSUBSCRIBE UINT16_C(5)
#define PXA_HOST_SYSTEM_SERVICE_REGISTER UINT16_C(6)
#define PXA_HOST_SYSTEM_SERVICE_UNREGISTER UINT16_C(7)
#define PXA_HOST_SYSTEM_SERVICE_COMPLETE UINT16_C(8)
#define PXA_HOST_SYSTEM_TOPIC_EVENT UINT16_C(0x8001)
#define PXA_HOST_SYSTEM_SERVICE_REQUEST UINT16_C(0x8002)

typedef struct {
    /* Canonical text key: <publisher-root-hex>:<app-id>. */
    char id[PXA_HOST_PACKAGE_ID_MAX];
    char app_id[PXA_HOST_APP_ID_MAX];
    char name[PXA_HOST_PACKAGE_NAME_MAX];
    char version[PXA_HOST_PACKAGE_VERSION_MAX];
    uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES];
    bool has_publisher_root;
    bool built_in;
    bool installed;
    bool staged;
    bool enabled;
    bool has_private_data;
    bool active;
    uint8_t permission_count;
    uint8_t granted_permission_count;
} pxa_host_package_info_t;

typedef struct {
    char name[PXA_HOST_PACKAGE_LOCALIZED_NAME_MAX + 1u];
    char description[PXA_HOST_PACKAGE_DESCRIPTION_MAX + 1u];
    char icon_path[PXA_HOST_PACKAGE_ICON_PATH_MAX + 1u];
} pxa_host_package_metadata_t;

/* Deployment diagnostics are populated only by
 * pxa_host_deploy_package_detailed(). status is a pxa_status_t value. */
typedef struct {
    int32_t status;
    char stage[PXA_HOST_PACKAGE_DEPLOY_STAGE_MAX];
} pxa_host_package_deploy_result_t;

typedef void (*pxa_host_icon_release_fn)(void *context);

typedef struct {
    const void *image_dsc;
    pxa_host_icon_release_fn release;
    void *release_context;
} pxa_host_icon_t;

typedef enum {
    PXA_HOST_APP_ACTION_INSTALL = 0,
    PXA_HOST_APP_ACTION_UNINSTALL,
    PXA_HOST_APP_ACTION_CLEAR_DATA,
    PXA_HOST_APP_ACTION_ENABLE,
    PXA_HOST_APP_ACTION_DISABLE,
} pxa_host_app_action_t;

/* Declared permission of an installed package. Entries keep manifest order, so
 * permission_index stays stable while the package remains installed. The text
 * buffer includes the NUL terminator. */
#define PXA_HOST_PERMISSION_TEXT_MAX 97

typedef struct {
    char name[PXA_HOST_PERMISSION_TEXT_MAX];
    char scope[PXA_HOST_PERMISSION_TEXT_MAX];
    bool required;
    bool granted;
} pxa_host_app_permission_t;

typedef enum {
    PXA_HOST_KEY_VOLUME_UP = 1,
    PXA_HOST_KEY_VOLUME_DOWN = 2,
    PXA_HOST_KEY_VOLUME_UP_RELEASED = 3,
    PXA_HOST_KEY_VOLUME_DOWN_RELEASED = 4,
} pxa_host_key_t;

typedef enum {
    PXA_HOST_COLOR_SCHEME_LIGHT = 0,
    PXA_HOST_COLOR_SCHEME_DARK,
} pxa_host_color_scheme_t;

typedef enum {
    PXA_HOST_RUNTIME_STARTED = 1,
    PXA_HOST_RUNTIME_START_FAILED,
    PXA_HOST_RUNTIME_STOPPED,
} pxa_host_runtime_event_t;

typedef void (*pxa_host_runtime_event_fn)(void *context,
                                          pxa_host_runtime_event_t event,
                                          const char *identity_key);
typedef void (*pxa_host_catalog_changed_fn)(void *context);
typedef void (*pxa_host_window_changed_fn)(
    void *context, const char *identity_key,
    const pxa_window_configuration_t *configuration);
typedef bool (*pxa_host_launch_request_fn)(
    void *context,
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *identity_key);
typedef struct {
    uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES];
    char app_id[PXA_HOST_APP_ID_MAX];
    char component_id[PXA_HOST_COMPONENT_ID_MAX];
    /* Opaque libpxa request-routing token; not part of system identity. */
    uint32_t runtime_component;
} pxa_host_system_caller_t;
typedef bool (*pxa_host_system_request_fn)(
    void *context, const pxa_host_system_caller_t *caller, uint16_t opcode,
    uint32_t request_id, const uint8_t *payload, size_t payload_size);

/* Public C facade over the ESP PXA host. */
bool pxa_host_initialize(void);
bool pxa_host_start_runtime(void);
bool pxa_host_scan_packages(void);
bool pxa_host_refresh_inbox(void);
bool pxa_host_stage_package_file(const char *source_path);
bool pxa_host_install_package_file(const char *source_path);
size_t pxa_host_package_count(void);
size_t pxa_host_list_packages(pxa_host_package_info_t *packages,
                              size_t capacity);
bool pxa_host_resolve_package_metadata(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *locale,
    pxa_host_package_metadata_t *metadata);
bool pxa_host_load_package_icon_path(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *icon_path, pxa_host_icon_t *icon);
void pxa_host_set_catalog_changed_callback(
    pxa_host_catalog_changed_fn callback, void *context);
void pxa_host_set_launch_request_callback(
    pxa_host_launch_request_fn callback, void *context);
void pxa_host_set_window_changed_callback(
    pxa_host_window_changed_fn callback, void *context);
bool pxa_host_request_launch(const char *identity_key);
/* Resolves an installed app ID and requests that its active runtime stops. */
bool pxa_host_request_stop(const char *identity_key);
bool pxa_host_runtime_launch(const char *identity_key);
bool pxa_host_runtime_launch_app(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id);
bool pxa_host_runtime_back(void);
bool pxa_host_runtime_stop(const char *identity_key);
bool pxa_host_active_identity(char *identity, size_t capacity);
bool pxa_host_deploy_package(const char *identity_key);
bool pxa_host_deploy_package_detailed(
    const char *identity_key, pxa_host_package_deploy_result_t *result);
bool pxa_host_manage_app(pxa_host_app_action_t action,
                         const char *identity_key);
/* Two-phase enumeration like pxa_host_list_packages: permissions == NULL
 * returns the declared permission count without touching capacity. */
size_t pxa_host_list_app_permissions(const char *identity_key,
                                     pxa_host_app_permission_t *permissions,
                                     size_t capacity);
/* Applies one declared permission; permission_index matches the list above. */
bool pxa_host_set_app_permission(const char *identity_key,
                                 size_t permission_index, bool granted);
bool pxa_host_ready(void);
bool pxa_host_is_active(const char *identity_key);
bool pxa_host_captures_volume_keys(void);
bool pxa_host_post_key(pxa_host_key_t key);
/* Thread-safe. Active and subsequently launched UI components inherit it. */
bool pxa_host_set_color_scheme(pxa_host_color_scheme_t color_scheme);
bool pxa_host_set_ui_palette(const uint32_t rgba[10]);
bool pxa_host_set_ui_palette_extended(const uint32_t rgba[32]);
/* Thread-safe locale snapshot used for startup configuration and subsequent
 * configuration events. locale is a canonical BCP 47 tag. */
bool pxa_host_set_locale(const char *locale, uint8_t text_direction);
/* Thread-safe display metrics handed to guest applications. safe_insets is
 * the physical safe area (cutouts, rounded corners); system_bar_insets is the
 * screen region covered or reserved by status/navigation chrome, including
 * the Home and Back gesture strips in gesture navigation mode. Both use
 * logical pixels and may be updated when chrome visibility changes. */
bool pxa_host_set_window_insets(const pxa_window_insets_t *safe_insets,
                                const pxa_window_insets_t *system_bar_insets);
/* Shape values match the PXSYS display profile. Radii are TL, TR, BR, BL. */
bool pxa_host_set_display_geometry(uint32_t shape, const uint16_t radii[4]);
/* Runtime events originate on the PXA worker. The consumer must marshal them
 * before mutating thread-confined application-system state. */
void pxa_host_set_runtime_event_callback(pxa_host_runtime_event_fn callback,
                                         void *context);
/* System requests originate on the PXA worker. The callback must copy the
 * request and marshal it to the application-system owner thread. */
void pxa_host_set_system_request_callback(pxa_host_system_request_fn callback,
                                          void *context);
/* Thread-safe completion; payload is copied before this function returns. */
bool pxa_host_complete_system_request(const char *app_id, uint32_t component,
                                      uint32_t request_id, int32_t status,
                                      const void *payload,
                                      size_t payload_size);
/* Thread-safe unsolicited event delivery; payload is copied. */
bool pxa_host_post_system_event(const char *app_id, uint32_t component,
                                uint16_t opcode, uint32_t request_id,
                                const void *payload,
                                size_t payload_size);
bool pxa_host_post_app_system_event(const char *identity_key, uint16_t opcode,
                                    const void *payload, size_t payload_size);

#define PXA_HOST_SYSTEM_INTENT_EVENT UINT16_C(0x8003)
#define PXA_HOST_SYSTEM_CONFIGURATION_EVENT UINT16_C(0x8004)
#define PXA_HOST_SYSTEM_LIFECYCLE_EVENT UINT16_C(0x8005)
#define PXA_HOST_SYSTEM_LIFECYCLE_BACKGROUND UINT8_C(0)
#define PXA_HOST_SYSTEM_LIFECYCLE_FOREGROUND UINT8_C(1)

#define PXA_HOST_CONTROLLER_UP (UINT32_C(1) << 0)
#define PXA_HOST_CONTROLLER_DOWN (UINT32_C(1) << 1)
#define PXA_HOST_CONTROLLER_LEFT (UINT32_C(1) << 2)
#define PXA_HOST_CONTROLLER_RIGHT (UINT32_C(1) << 3)
#define PXA_HOST_CONTROLLER_A (UINT32_C(1) << 4)
#define PXA_HOST_CONTROLLER_B (UINT32_C(1) << 5)
#define PXA_HOST_CONTROLLER_START (UINT32_C(1) << 6)
#define PXA_HOST_CONTROLLER_SELECT (UINT32_C(1) << 7)
#define PXA_HOST_CONTROLLER_BUTTON_MASK UINT32_C(0xff)

bool pxa_host_post_controller_state(uint8_t controller, bool connected,
                                    uint32_t buttons);

/* The application owns the physical audio renderer. The PXA host invokes
 * these callbacks from its audio worker after a Guest PCM frame is copied
 * out of Wasm memory. */
typedef bool (*pxa_host_audio_submit_fn)(void *context, uint8_t voice,
                                         const int16_t *pcm, size_t samples);
typedef void (*pxa_host_audio_flush_fn)(void *context, uint8_t voice);
void pxa_host_set_audio_sink(pxa_host_audio_submit_fn submit,
                             pxa_host_audio_flush_fn flush,
                             void *context);

/* Asset callbacks run on the PXA runtime stack and must only copy/enqueue the
 * command. The absolute package path is valid only until play returns. */
typedef bool (*pxa_host_audio_asset_play_fn)(
    void *context, uint8_t voice, const char *absolute_path, bool loop,
    int16_t gain_db_q8);
typedef bool (*pxa_host_audio_asset_control_fn)(
    void *context, uint8_t voice, uint8_t action, int16_t gain_db_q8);
void pxa_host_set_audio_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context);

#ifdef __cplusplus
}
#endif

#endif
