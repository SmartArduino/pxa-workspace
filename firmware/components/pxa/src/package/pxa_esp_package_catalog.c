#include "pxa_esp_package_catalog.h"

#if defined(ESP_PLATFORM)

#include <stdio.h>
#include <string.h>

#include "app_default_icon.h"
#include "pxa_esp_package_icon.h"
#include "pxa_esp_package_store.h"

typedef char pxa_esp_catalog_id_must_fit[
    PXA_ESP_PACKAGE_ID_BYTES <= APP_PAGES_APP_ID_MAX ? 1 : -1];
typedef char pxa_esp_catalog_name_must_fit[
    PXA_ESP_PACKAGE_NAME_BYTES <= APP_PAGES_APP_NAME_MAX + 1u ? 1 : -1];
typedef char pxa_esp_catalog_version_must_fit[
    PXA_ESP_PACKAGE_VERSION_BYTES <= APP_PAGES_APP_VERSION_MAX ? 1 : -1];
typedef char pxa_esp_catalog_permission_name_must_fit[
    PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES <=
            APP_PAGES_APP_PERMISSION_NAME_MAX
        ? 1
        : -1];
typedef char pxa_esp_catalog_permission_scope_must_fit[
    PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES <=
            APP_PAGES_APP_PERMISSION_SCOPE_MAX
        ? 1
        : -1];

typedef struct {
    app_pages_app_t *apps;
    size_t capacity;
    size_t count;
} catalog_projection_t;

typedef struct {
    app_pages_managed_app_t *apps;
    size_t capacity;
    size_t count;
} manager_projection_t;

typedef struct {
    app_pages_app_permission_t *permissions;
    size_t capacity;
    size_t count;
} permission_projection_t;

static bool project_catalog_record(const pxa_esp_package_record_t *record,
                                   void *user_data) {
    catalog_projection_t *projection = user_data;
    app_pages_app_t *app;
    if (record == NULL || projection == NULL ||
        projection->count >= projection->capacity) {
        return false;
    }
    app = &projection->apps[projection->count++];
    memset(app, 0, sizeof(*app));
    snprintf(app->id, sizeof(app->id), "%s", record->id);
    snprintf(app->name, sizeof(app->name), "%s", record->name);
    snprintf(app->version, sizeof(app->version), "%s", record->version);
    app->runnable = true;
    return projection->count < projection->capacity;
}

static bool project_managed_record(const pxa_esp_package_record_t *record,
                                   void *user_data) {
    manager_projection_t *projection = user_data;
    app_pages_managed_app_t *app;
    if (record == NULL || projection == NULL ||
        projection->count >= projection->capacity) {
        return false;
    }
    app = &projection->apps[projection->count++];
    memset(app, 0, sizeof(*app));
    snprintf(app->id, sizeof(app->id), "%s", record->id);
    snprintf(app->name, sizeof(app->name), "%s", record->name);
    snprintf(app->version, sizeof(app->version), "%s", record->version);
    snprintf(app->staged_version, sizeof(app->staged_version), "%s",
             record->staged_version);
    app->built_in = record->built_in;
    app->installed = record->installed;
    app->staged = record->staged;
    app->enabled = record->enabled;
    app->has_private_data = record->has_private_data;
    app->has_release_sequence = record->has_release_sequence;
    app->has_staged_release_sequence = record->has_staged_release_sequence;
    app->downgrade_warning = record->downgrade_warning;
    app->signer_rollback_warning = record->signer_rollback_warning;
    app->release_sequence = record->release_sequence;
    app->staged_release_sequence = record->staged_release_sequence;
    app->permission_count = record->permission_count;
    app->granted_permission_count = record->granted_permission_count;
    return projection->count < projection->capacity;
}

static bool project_permission(
    const pxa_esp_package_permission_info_t *permission, void *user_data) {
    permission_projection_t *projection = user_data;
    app_pages_app_permission_t *app_permission;
    if (permission == NULL || projection == NULL ||
        projection->count >= projection->capacity) {
        return false;
    }
    app_permission = &projection->permissions[projection->count++];
    memset(app_permission, 0, sizeof(*app_permission));
    snprintf(app_permission->name, sizeof(app_permission->name), "%s",
             permission->name);
    snprintf(app_permission->scope, sizeof(app_permission->scope), "%s",
             permission->scope);
    app_permission->required = permission->required;
    app_permission->granted = permission->granted;
    return projection->count < projection->capacity;
}

static app_pages_app_icon_t load_package_icon(
    const char *identity, pxa_esp_package_icon_variant_t variant) {
    app_pages_app_icon_t result;
    pxa_host_icon_t icon;
    pxa_esp_package_icon_source_t source;
    memset(&result, 0, sizeof(result));
    memset(&source, 0, sizeof(source));
    if (!pxa_esp_package_store_icon_source(identity, variant, &source)) {
        result.image_dsc = &app_default_icon;
        return result;
    }
    icon = pxa_esp_package_icon_load(source.root, source.relative_path);
    result.image_dsc = (const lv_image_dsc_t *)icon.image_dsc;
    result.release_cb = icon.release;
    result.release_user_data = icon.release_context;
    if (result.image_dsc == NULL) result.image_dsc = &app_default_icon;
    return result;
}

size_t pxa_esp_package_catalog_apps(app_pages_app_t *apps, size_t capacity) {
    catalog_projection_t projection;
    size_t index;
    if (apps == NULL && capacity == 0) {
        return pxa_esp_package_store_count(PXA_ESP_PACKAGE_VIEW_RUNNABLE);
    }
    if (apps == NULL || capacity == 0) return 0;
    projection.apps = apps;
    projection.capacity = capacity;
    projection.count = 0;
    (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_RUNNABLE,
                                      project_catalog_record, &projection);
    for (index = 0; index < projection.count; ++index) {
        apps[index].icon = load_package_icon(
            apps[index].id, PXA_ESP_PACKAGE_ICON_INSTALLED);
    }
    return projection.count;
}

size_t pxa_esp_package_catalog_managed(app_pages_managed_app_t *apps,
                                       size_t capacity) {
    manager_projection_t projection;
    size_t index;
    if (apps == NULL && capacity == 0) {
        return pxa_esp_package_store_count(PXA_ESP_PACKAGE_VIEW_MANAGED);
    }
    if (apps == NULL || capacity == 0) return 0;
    projection.apps = apps;
    projection.capacity = capacity;
    projection.count = 0;
    (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_MANAGED,
                                      project_managed_record, &projection);
    for (index = 0; index < projection.count; ++index) {
        apps[index].icon = load_package_icon(
            apps[index].id,
            apps[index].installed ? PXA_ESP_PACKAGE_ICON_INSTALLED
                                  : PXA_ESP_PACKAGE_ICON_STAGED);
    }
    return projection.count;
}

size_t pxa_esp_package_catalog_permissions(
    const char *identity, app_pages_app_permission_t *permissions,
    size_t capacity) {
    permission_projection_t projection;
    if (identity == NULL) return 0;
    if (permissions == NULL && capacity == 0) {
        return pxa_esp_package_store_permission_count(identity);
    }
    if (permissions == NULL || capacity == 0) return 0;
    projection.permissions = permissions;
    projection.capacity = capacity;
    projection.count = 0;
    (void)pxa_esp_package_store_visit_permissions(
        identity, project_permission, &projection);
    return projection.count;
}

#endif /* ESP_PLATFORM */
