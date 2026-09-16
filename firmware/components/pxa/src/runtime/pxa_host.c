#include "sdkconfig.h"

#if defined(ESP_PLATFORM) && CONFIG_PXA_ENABLED

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pxa/pxa_host.h"
#include "pxa/package_icon.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "pxa_esp_host.h"
#include "pxa_esp_package_icon.h"
#include "pxa_esp_package_store.h"
#include "pxa_esp_ui_shell.h"

#define PXA_HOST_TAG "PxaHost"

static bool g_host_ready;
static bool g_window_insets_valid;
static pxa_window_insets_t g_safe_insets;
static pxa_window_insets_t g_system_bar_insets;
static pxa_host_catalog_changed_fn g_catalog_changed_callback;
static void *g_catalog_changed_context;
static pxa_host_launch_request_fn g_launch_request_callback;
static void *g_launch_request_context;
static portMUX_TYPE g_callback_lock = portMUX_INITIALIZER_UNLOCKED;

static bool format_identity_key(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id, char output[PXA_HOST_PACKAGE_ID_MAX]) {
    static const char digits[] = "0123456789abcdef";
    size_t app_id_size;
    size_t offset = 0;
    size_t index;
    if (publisher_root == NULL || app_id == NULL || output == NULL)
        return false;
    app_id_size = strlen(app_id);
    if (app_id_size == 0 || app_id_size >= PXA_HOST_APP_ID_MAX ||
        PXA_HOST_PUBLISHER_ROOT_BYTES * 2u + 1u + app_id_size + 1u >
            PXA_HOST_PACKAGE_ID_MAX) {
        return false;
    }
    for (index = 0; index < PXA_HOST_PUBLISHER_ROOT_BYTES; ++index) {
        output[offset++] = digits[publisher_root[index] >> 4];
        output[offset++] = digits[publisher_root[index] & 0x0fu];
    }
    output[offset++] = ':';
    memcpy(output + offset, app_id, app_id_size + 1u);
    return true;
}

static bool resolve_package(
    const char *identity_key,
    uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    char *canonical, size_t canonical_capacity,
    char *app_id, size_t app_id_capacity) {
    pxa_host_package_info_t *packages;
    const pxa_host_package_info_t *match = NULL;
    size_t count;
    size_t index;
    bool found = false;
    bool ambiguous = false;
    if (identity_key == NULL || publisher_root == NULL || canonical == NULL ||
        canonical_capacity == 0 || app_id == NULL || app_id_capacity == 0) {
        return false;
    }
    count = pxa_esp_host_package_count();
    if (count == 0 || count > SIZE_MAX / sizeof(*packages)) return false;
    packages = calloc(count, sizeof(*packages));
    if (packages == NULL) return false;
    count = pxa_esp_host_list_packages(packages, count);
    for (index = 0; index < count; ++index) {
        if (!packages[index].installed || !packages[index].has_publisher_root)
            continue;
        if (strcmp(packages[index].id, identity_key) == 0) {
            match = &packages[index];
            ambiguous = false;
            break;
        }
        if (strcmp(packages[index].app_id, identity_key) == 0) {
            if (match != NULL) {
                ambiguous = true;
            } else {
                match = &packages[index];
            }
        }
    }
    if (!ambiguous && match != NULL) {
        memcpy(publisher_root, match->publisher_root,
               PXA_HOST_PUBLISHER_ROOT_BYTES);
        snprintf(canonical, canonical_capacity, "%s", match->id);
        snprintf(app_id, app_id_capacity, "%s", match->app_id);
        found = canonical[0] != '\0' && app_id[0] != '\0';
    }
    free(packages);
    return found;
}

static void notify_catalog_changed(void) {
    pxa_host_catalog_changed_fn callback;
    void *context;
    pxa_esp_ui_shell_refresh_apps();
    portENTER_CRITICAL(&g_callback_lock);
    callback = g_catalog_changed_callback;
    context = g_catalog_changed_context;
    portEXIT_CRITICAL(&g_callback_lock);
    if (callback != NULL) callback(context);
}

static void package_store_synced(void *context) {
    (void)context;
    notify_catalog_changed();
}

bool pxa_host_initialize(void) {
    bool publish_insets;
    pxa_window_insets_t safe_insets;
    pxa_window_insets_t system_bar_insets;
    if (g_host_ready) return true;
    g_host_ready = pxa_esp_host_initialize();
    if (!g_host_ready) {
        ESP_LOGE(PXA_HOST_TAG, "PXA component runtime initialization failed");
    } else {
        portENTER_CRITICAL(&g_callback_lock);
        publish_insets = g_window_insets_valid;
        safe_insets = g_safe_insets;
        system_bar_insets = g_system_bar_insets;
        portEXIT_CRITICAL(&g_callback_lock);
        if (publish_insets) {
            (void)pxa_esp_host_set_window_insets(&safe_insets,
                                                 &system_bar_insets);
        }
        pxa_esp_package_store_set_sync_callback(package_store_synced, NULL);
        notify_catalog_changed();
    }
    return g_host_ready;
}

bool pxa_host_start_runtime(void) {
    return g_host_ready && pxa_esp_host_start_runtime();
}

bool pxa_host_scan_packages(void) {
    bool success;
    if (!g_host_ready) return false;
    success = pxa_esp_host_refresh_inbox();
    pxa_esp_host_sync_builtins();
    return success;
}

bool pxa_host_refresh_inbox(void) {
    bool success;
    if (!g_host_ready) return false;
    success = pxa_esp_host_refresh_inbox();
    if (success) notify_catalog_changed();
    return success;
}

bool pxa_host_stage_package_file(const char *source_path) {
    bool success;
    if (!g_host_ready || source_path == NULL || source_path[0] == '\0') {
        return false;
    }
    success = pxa_esp_host_stage_package_file(source_path);
    if (success) notify_catalog_changed();
    return success;
}

bool pxa_host_install_package_file(const char *source_path) {
    bool success;
    if (!g_host_ready || source_path == NULL || source_path[0] == '\0')
        return false;
    success = pxa_esp_host_install_package_file(source_path);
    if (success) notify_catalog_changed();
    return success;
}

size_t pxa_host_package_count(void) {
    if (!g_host_ready) return 0;
    return pxa_esp_host_package_count();
}

size_t pxa_host_list_packages(pxa_host_package_info_t *packages,
                              size_t capacity) {
    if (!g_host_ready) return 0;
    return pxa_esp_host_list_packages(packages, capacity);
}

bool pxa_host_resolve_package_metadata(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *locale,
    pxa_host_package_metadata_t *metadata) {
    char identity[PXA_HOST_PACKAGE_ID_MAX];
    if (!g_host_ready || locale == NULL || metadata == NULL ||
        !format_identity_key(publisher_root, app_id, identity)) {
        return false;
    }
    return pxa_esp_host_resolve_package_metadata(identity, locale, metadata);
}

bool pxa_host_load_package_icon_path(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *icon_path, pxa_host_icon_t *icon) {
    char identity[PXA_HOST_PACKAGE_ID_MAX];
    pxa_esp_package_icon_source_t source;
    if (icon == NULL) return false;
    memset(icon, 0, sizeof(*icon));
    if (!g_host_ready || icon_path == NULL ||
        !format_identity_key(publisher_root, app_id, identity) ||
        !pxa_esp_package_store_icon_source(
            identity, PXA_ESP_PACKAGE_ICON_INSTALLED, &source)) {
        return false;
    }
    *icon = pxa_esp_package_icon_load(source.root, icon_path);
    return icon->image_dsc != NULL;
}

void pxa_host_set_catalog_changed_callback(
    pxa_host_catalog_changed_fn callback, void *context) {
    portENTER_CRITICAL(&g_callback_lock);
    g_catalog_changed_callback = callback;
    g_catalog_changed_context = context;
    portEXIT_CRITICAL(&g_callback_lock);
    if (g_host_ready && callback != NULL) callback(context);
}

void pxa_host_set_launch_request_callback(
    pxa_host_launch_request_fn callback, void *context) {
    portENTER_CRITICAL(&g_callback_lock);
    g_launch_request_callback = callback;
    g_launch_request_context = context;
    portEXIT_CRITICAL(&g_callback_lock);
}

bool pxa_host_request_launch(const char *identity_key) {
    pxa_host_launch_request_fn callback;
    void *context;
    uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES];
    char canonical[PXA_HOST_PACKAGE_ID_MAX];
    char app_id[PXA_HOST_APP_ID_MAX];
    if (!g_host_ready || identity_key == NULL || identity_key[0] == '\0')
        return false;
    portENTER_CRITICAL(&g_callback_lock);
    callback = g_launch_request_callback;
    context = g_launch_request_context;
    portEXIT_CRITICAL(&g_callback_lock);
    if (!resolve_package(identity_key, publisher_root, canonical,
                         sizeof(canonical), app_id, sizeof(app_id))) {
        return false;
    }
    if (callback != NULL)
        return callback(context, publisher_root, app_id);
    return pxa_esp_host_launch(canonical);
}

bool pxa_host_request_stop(const char *identity_key) {
    uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES];
    char canonical[PXA_HOST_PACKAGE_ID_MAX];
    char app_id[PXA_HOST_APP_ID_MAX];
    if (!g_host_ready || identity_key == NULL || identity_key[0] == '\0')
        return false;
    if (!resolve_package(identity_key, publisher_root, canonical,
                         sizeof(canonical), app_id, sizeof(app_id))) {
        return false;
    }
    return pxa_esp_host_is_active(canonical) && pxa_esp_host_stop(canonical);
}

bool pxa_host_runtime_launch(const char *identity_key) {
    return g_host_ready && identity_key != NULL && identity_key[0] != '\0' &&
           pxa_esp_host_launch(identity_key);
}

bool pxa_host_runtime_launch_app(
    const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
    const char *app_id) {
    char canonical[PXA_HOST_PACKAGE_ID_MAX];
    return g_host_ready && publisher_root != NULL && app_id != NULL &&
           app_id[0] != '\0' && format_identity_key(publisher_root, app_id,
                                                    canonical) &&
           pxa_esp_host_launch(canonical);
}

bool pxa_host_runtime_back(void) {
    return g_host_ready && pxa_esp_host_back();
}

bool pxa_host_runtime_stop(const char *identity_key) {
    return g_host_ready && pxa_esp_host_stop(identity_key);
}

bool pxa_host_deploy_package(const char *identity_key) {
    bool success;
    if (!g_host_ready || identity_key == NULL || identity_key[0] == '\0') {
        return false;
    }
    success = pxa_esp_host_deploy_package(identity_key);
    if (success) notify_catalog_changed();
    return success;
}

bool pxa_host_manage_app(pxa_host_app_action_t action,
                         const char *identity_key) {
    bool success;
    if (!g_host_ready || identity_key == NULL || identity_key[0] == '\0') {
        return false;
    }
    success = pxa_esp_host_manage_app(action, identity_key);
    if (success) notify_catalog_changed();
    return success;
}

bool pxa_host_ready(void) {
    return g_host_ready;
}

bool pxa_host_is_active(const char *identity_key) {
    return g_host_ready && pxa_esp_host_is_active(identity_key);
}

bool pxa_host_captures_volume_keys(void) {
    return g_host_ready && pxa_esp_host_captures_volume_keys();
}

bool pxa_host_post_key(pxa_host_key_t key) {
    if (!g_host_ready || key < PXA_HOST_KEY_VOLUME_UP ||
        key > PXA_HOST_KEY_VOLUME_DOWN_RELEASED) {
        return false;
    }
    return pxa_esp_host_post_key((uint16_t)key);
}

bool pxa_host_set_color_scheme(pxa_host_color_scheme_t color_scheme) {
    return g_host_ready && pxa_esp_host_set_color_scheme(color_scheme);
}

bool pxa_host_set_locale(const char *locale, uint8_t text_direction) {
    return g_host_ready && pxa_esp_host_set_locale(locale, text_direction);
}

bool pxa_host_set_window_insets(const pxa_window_insets_t *safe_insets,
                                const pxa_window_insets_t *system_bar_insets) {
    bool ready;
    pxa_window_insets_t safe;
    pxa_window_insets_t bars;
    if (safe_insets == NULL && system_bar_insets == NULL) return false;
    portENTER_CRITICAL(&g_callback_lock);
    if (safe_insets != NULL) g_safe_insets = *safe_insets;
    if (system_bar_insets != NULL) g_system_bar_insets = *system_bar_insets;
    g_window_insets_valid = true;
    safe = g_safe_insets;
    bars = g_system_bar_insets;
    ready = g_host_ready;
    portEXIT_CRITICAL(&g_callback_lock);
    return !ready || pxa_esp_host_set_window_insets(&safe, &bars);
}

void pxa_host_set_runtime_event_callback(pxa_host_runtime_event_fn callback,
                                         void *context) {
    pxa_esp_host_set_runtime_event_callback(callback, context);
}

void pxa_host_set_window_changed_callback(
    pxa_host_window_changed_fn callback, void *context) {
    pxa_esp_host_set_window_changed_callback(callback, context);
}

void pxa_host_set_system_request_callback(pxa_host_system_request_fn callback,
                                          void *context) {
    pxa_esp_host_set_system_request_callback(callback, context);
}

bool pxa_host_complete_system_request(const char *app_id, uint32_t component,
                                      uint32_t request_id, int32_t status,
                                      const void *payload,
                                      size_t payload_size) {
    return pxa_esp_host_complete_system_request(
        app_id, component, request_id, status, payload, payload_size);
}

bool pxa_host_post_system_event(const char *app_id, uint32_t component,
                                uint16_t opcode, uint32_t request_id,
                                const void *payload,
                                size_t payload_size) {
    return pxa_esp_host_post_system_event(app_id, component, opcode, request_id,
                                          payload, payload_size);
}

bool pxa_host_post_app_system_event(const char *identity_key, uint16_t opcode,
                                    const void *payload, size_t payload_size) {
    return pxa_esp_host_post_app_system_event(identity_key, opcode, payload,
                                              payload_size);
}

bool pxa_host_post_controller_state(uint8_t controller, bool connected,
    uint32_t buttons) {
    if (!g_host_ready || (!connected && buttons != 0) ||
        (buttons & ~PXA_HOST_CONTROLLER_BUTTON_MASK) != 0)
        return false;
    return pxa_esp_host_post_controller_state(controller, connected, buttons);
}

void pxa_host_set_audio_sink(pxa_host_audio_submit_fn submit,
                             pxa_host_audio_flush_fn flush,
                             void *context) {
    pxa_esp_host_set_audio_sink(submit, flush, context);
}

void pxa_host_set_audio_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context) {
    pxa_esp_host_set_audio_asset_sink(play, control, context);
}

#endif
