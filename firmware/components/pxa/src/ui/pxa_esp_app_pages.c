#include "pxa_esp_ui_shell.h"

#if defined(ESP_PLATFORM)

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_default_icon.h"
#include "app_pages.h"
#include "pxa/pxa_host.h"
#include "pxa/pxa_esp_surface.h"

#include "pxa_esp_host.h"
#include "pxa_esp_package_catalog.h"
#include "pxa_esp_package_icon.h"
#include "pxa_esp_package_store.h"

static app_pages_app_icon_t app_pages_icon(pxa_host_icon_t source) {
    app_pages_app_icon_t result;
    memset(&result, 0, sizeof(result));
    result.image_dsc = (const lv_image_dsc_t *)source.image_dsc;
    result.release_cb = source.release;
    result.release_user_data = source.release_context;
    if (result.image_dsc == NULL) result.image_dsc = &app_default_icon;
    return result;
}

typedef char pxa_esp_ui_shell_app_name_must_fit_app_pages[
    PXA_ESP_UI_SHELL_APP_NAME_BYTES <= APP_PAGES_APP_NAME_MAX ? 1 : -1];
typedef char pxa_esp_ui_shell_permission_text_must_fit_app_pages[
    PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES <=
            APP_PAGES_APP_PERMISSION_NAME_MAX &&
        PXA_ESP_UI_SHELL_PERMISSION_TEXT_BYTES <=
            APP_PAGES_APP_PERMISSION_SCOPE_MAX
        ? 1
        : -1];

static size_t catalog_cb(app_pages_app_t *apps, size_t capacity,
                         void *user_data) {
    (void)user_data;
    return pxa_esp_package_catalog_apps(apps, capacity);
}

static bool launch_cb(const char *identity, void *user_data) {
    (void)user_data;
    return pxa_host_request_launch(identity);
}

static size_t manager_cb(app_pages_managed_app_t *apps, size_t capacity,
                         void *user_data) {
    char active_identity[PXA_HOST_PACKAGE_ID_MAX];
    int has_active;
    size_t count;
    size_t index;
    (void)user_data;
    if (apps == NULL && capacity == 0) {
        return pxa_esp_package_catalog_managed(NULL, 0);
    }
    has_active = pxa_esp_host_active_identity(active_identity,
                                              sizeof(active_identity));
    count = pxa_esp_package_catalog_managed(apps, capacity);
    for (index = 0; index < count; ++index) {
        apps[index].active =
            has_active && strcmp(active_identity, apps[index].id) == 0;
    }
    return count;
}

static int map_action(app_pages_app_action_t action,
                      pxa_host_app_action_t *mapped) {
    if (mapped == NULL) return 0;
    switch (action) {
        case APP_PAGES_APP_ACTION_INSTALL:
            *mapped = PXA_HOST_APP_ACTION_INSTALL;
            return 1;
        case APP_PAGES_APP_ACTION_UNINSTALL:
            *mapped = PXA_HOST_APP_ACTION_UNINSTALL;
            return 1;
        case APP_PAGES_APP_ACTION_CLEAR_DATA:
            *mapped = PXA_HOST_APP_ACTION_CLEAR_DATA;
            return 1;
        case APP_PAGES_APP_ACTION_ENABLE:
            *mapped = PXA_HOST_APP_ACTION_ENABLE;
            return 1;
        case APP_PAGES_APP_ACTION_DISABLE:
            *mapped = PXA_HOST_APP_ACTION_DISABLE;
            return 1;
        default:
            return 0;
    }
}

static bool action_cb(app_pages_app_action_t action, const char *identity,
                      void *user_data) {
    pxa_host_app_action_t mapped;
    bool success;
    (void)user_data;
    if (!map_action(action, &mapped)) return false;
    success = pxa_host_manage_app(mapped, identity);
    if (action == APP_PAGES_APP_ACTION_UNINSTALL)
        pxa_esp_ui_shell_refresh_apps();
    return success;
}

static bool install_package_cb(const char *path, void *user_data) {
    bool success;
    (void)user_data;
    success = pxa_host_install_package_file(path);
    return success;
}

static bool package_preview_cb(const char *path,
                               app_pages_package_preview_t *preview,
                               void *user_data) {
    pxa_esp_package_preview_t package;
    pxa_esp_package_icon_source_t icon_source;
    (void)user_data;
    if (preview == NULL ||
        !pxa_esp_package_store_preview_file(path, &package)) {
        return false;
    }
    snprintf(preview->app.id, sizeof(preview->app.id), "%s", package.id);
    snprintf(preview->app.name, sizeof(preview->app.name), "%s", package.name);
    snprintf(preview->app.version, sizeof(preview->app.version), "%s",
             package.version);
    snprintf(preview->description, sizeof(preview->description), "%s",
             package.description);
    preview->permission_count = package.permission_count;
    for (size_t index = 0; index < preview->permission_count; ++index) {
        snprintf(preview->permissions[index].name,
                 sizeof(preview->permissions[index].name), "%s",
                 package.permissions[index].name);
        snprintf(preview->permissions[index].scope,
                 sizeof(preview->permissions[index].scope), "%s",
                 package.permissions[index].scope);
        preview->permissions[index].required =
            package.permissions[index].required;
    }
    preview->app.icon = app_pages_icon(pxa_esp_package_icon_default());
    memset(&icon_source, 0, sizeof(icon_source));
    if (pxa_esp_package_store_icon_source(
            package.id, PXA_ESP_PACKAGE_ICON_INSTALLED, &icon_source)) {
        preview->app.icon = app_pages_icon(pxa_esp_package_icon_load(
            icon_source.root, icon_source.relative_path));
    }
    return true;
}

static size_t permission_list_cb(
    const char *identity, app_pages_app_permission_t *permissions,
    size_t capacity, void *user_data) {
    (void)user_data;
    return pxa_esp_package_catalog_permissions(identity, permissions,
                                               capacity);
}

static bool permission_set_cb(const char *identity, size_t permission_index,
                              bool granted, void *user_data) {
    (void)user_data;
    return pxa_esp_host_set_permission(identity, permission_index, granted);
}

static bool permission_response_cb(uint32_t prompt_id, bool granted,
                                   void *user_data) {
    (void)user_data;
    return pxa_esp_host_respond_permission(prompt_id, granted);
}

static bool unresponsive_response_cb(uint32_t prompt_id, bool wait,
                                     void *user_data) {
    (void)user_data;
    return pxa_esp_host_respond_unresponsive(prompt_id, wait);
}

static bool back_cb(void *user_data) {
    (void)user_data;
    return pxa_esp_host_back();
}

static void system_overlay_visibility_cb(bool visible, void *user_data) {
    (void)user_data;
    pxa_esp_surface_set_system_overlay_visible(visible);
}

void pxa_esp_ui_shell_bind(void) {
    app_pages_set_app_catalog_callbacks(catalog_cb, launch_cb, NULL);
    app_pages_set_app_manager_callbacks(manager_cb, action_cb, NULL);
    app_pages_set_package_install_callback(install_package_cb, NULL);
    app_pages_set_package_preview_callback(package_preview_cb, NULL);
    app_pages_set_app_permission_callbacks(permission_list_cb,
                                           permission_set_cb, NULL);
    app_pages_set_runtime_permission_response_callback(permission_response_cb,
                                                       NULL);
    app_pages_set_unresponsive_response_callback(unresponsive_response_cb,
                                                 NULL);
    app_pages_set_back_request_callback(back_cb, NULL);
    app_pages_set_system_overlay_visibility_callback(
        system_overlay_visibility_cb, NULL);
}

int pxa_esp_ui_shell_post_permission_prompt(
    const pxa_esp_ui_permission_prompt_t *prompt) {
    app_pages_runtime_permission_prompt_t app_prompt;
    if (prompt == NULL) return 0;
    memset(&app_prompt, 0, sizeof(app_prompt));
    app_prompt.prompt_id = prompt->prompt_id;
    snprintf(app_prompt.app_name, sizeof(app_prompt.app_name), "%s",
             prompt->app_name);
    snprintf(app_prompt.permission_name, sizeof(app_prompt.permission_name),
             "%s", prompt->permission_name);
    snprintf(app_prompt.scope, sizeof(app_prompt.scope), "%s", prompt->scope);
    return app_pages_post_runtime_permission_prompt(&app_prompt) ? 1 : 0;
}

void pxa_esp_ui_shell_dismiss_permission_prompt(uint32_t prompt_id) {
    app_pages_dismiss_runtime_permission_prompt(prompt_id);
}

int pxa_esp_ui_shell_post_unresponsive_prompt(
    const pxa_esp_ui_unresponsive_prompt_t *prompt) {
    app_pages_unresponsive_prompt_t app_prompt;
    if (prompt == NULL) return 0;
    memset(&app_prompt, 0, sizeof(app_prompt));
    app_prompt.prompt_id = prompt->prompt_id;
    snprintf(app_prompt.app_name, sizeof(app_prompt.app_name), "%s",
             prompt->app_name);
    return app_pages_post_unresponsive_prompt(&app_prompt) ? 1 : 0;
}

void pxa_esp_ui_shell_dismiss_unresponsive_prompt(uint32_t prompt_id) {
    app_pages_dismiss_unresponsive_prompt(prompt_id);
}

void pxa_esp_ui_shell_post_toast(const char *message, uint32_t duration_ms) {
    int duration = duration_ms > (uint32_t)INT_MAX ? INT_MAX : (int)duration_ms;
    app_pages_post_toast(message, duration);
}

void pxa_esp_ui_shell_dismiss_app_launch(void) {
    app_pages_dismiss_app_launch();
}

void pxa_esp_ui_shell_refresh_apps(void) {
    app_pages_request_apps_refresh();
}

const lv_font_t *pxa_esp_ui_shell_text_font(void) {
    return app_pages_get_text_font();
}

const lv_font_t *pxa_esp_ui_shell_title_font(void) {
    return app_pages_get_title_font();
}

const lv_font_t *pxa_esp_ui_shell_icon_font(void) {
    return app_pages_get_icon_font();
}

#endif
