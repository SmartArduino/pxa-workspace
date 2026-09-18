#include "pxsys/esp_pxa_bridge.h"

#include "sdkconfig.h"

#if CONFIG_PXA_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "pxa/package_icon.h"
#include "pxa/pxa_esp_surface.h"
#include "pxa/pxa_host.h"
#include "pxsys/pxa_gateway_wire.h"
#include "pxsys/pxa_runtime.h"

#define PXSYS_ESP_PXA_MAGIC UINT32_C(0x50584550)
#define PXSYS_ESP_PXA_RUNTIME_ID "pxa-esp"
#define PXSYS_ESP_PXA_TAG "PxaBridge"

typedef struct {
    pxsys_app_identity_t identity;
    char app_id[PXA_HOST_APP_ID_MAX];
    char identity_key[PXA_HOST_PACKAGE_ID_MAX];
    char resource_namespace[PXA_HOST_APP_ID_MAX + 4u];
} tracked_app_t;

typedef struct esp_pxa_instance {
    struct pxsys_esp_pxa_bridge* bridge;
    struct esp_pxa_instance* next;
    pxsys_instance_ref_t reference;
    uint8_t publisher_root[PXSYS_PUBLISHER_ROOT_BYTES];
    char app_id[PXA_HOST_APP_ID_MAX];
    char identity_key[PXA_HOST_PACKAGE_ID_MAX];
    uint8_t* launch_event;
    size_t launch_event_size;
} esp_pxa_instance_t;

typedef struct {
    uint32_t generation;
    pxa_host_runtime_event_t event;
    char identity[PXA_HOST_PACKAGE_ID_MAX];
} runtime_event_t;

typedef struct {
    uint32_t generation;
    uint32_t component;
    uint32_t request_id;
    uint16_t opcode;
    size_t payload_size;
    uint8_t invoke_returned;
    uint8_t completed;
    pxa_host_system_caller_t caller;
    uint8_t payload[];
} system_request_t;

typedef struct {
    uint32_t generation;
    uint32_t window_generation;
    pxa_window_configuration_t configuration;
} window_change_t;

typedef struct {
    struct pxsys_esp_pxa_bridge* bridge;
    uint32_t generation;
    pxa_host_system_caller_t caller;
    pxsys_version_t minimum_version;
    char* topic;
    uint8_t occupied;
} pxa_subscription_t;

typedef struct {
    struct pxsys_esp_pxa_bridge* bridge;
    uint32_t generation;
    pxa_host_system_caller_t caller;
    pxsys_version_t version;
    uint64_t features;
    char* interface_id;
    uint8_t occupied;
} pxa_endpoint_t;

typedef struct {
    pxa_endpoint_t* endpoint;
    void* completion_context;
    pxsys_service_complete_fn complete;
    uint64_t call_id;
    uint64_t request_id;
    uint8_t occupied;
} pxa_pending_call_t;

struct pxsys_esp_pxa_bridge {
    uint32_t magic;
    uint32_t generation;
    uint32_t window_generation;
    size_t capacity;
    size_t subscription_capacity;
    size_t endpoint_capacity;
    size_t pending_call_capacity;
    size_t tracked_count;
    pxsys_allocator_t allocator;
    pxsys_standard_system_t* system;
    pxsys_pxa_runtime_t* pxa_runtime;
    void* catalog_synced_context;
    void (*catalog_synced)(void* context);
    esp_pxa_instance_t* instances;
    tracked_app_t* tracked;
    tracked_app_t* next;
    pxa_host_package_metadata_t* metadata_cache;
    uint8_t* metadata_valid;
    pxa_host_package_info_t* packages;
    pxa_subscription_t* subscriptions;
    pxa_endpoint_t* endpoints;
    pxa_pending_call_t* pending_calls;
    uint64_t next_call_id;
    uint8_t theme_subscribed;
    uint8_t locale_subscribed;
    uint8_t window_accepting;
    char metadata_locale[PXSYS_LOCALE_TAG_MAX_BYTES + 1u];
};

static pxsys_esp_pxa_bridge_t* g_bridge;
static uint32_t g_generation;
static portMUX_TYPE g_bridge_lock = portMUX_INITIALIZER_UNLOCKED;

static int bridge_valid(const pxsys_esp_pxa_bridge_t* bridge) {
    return bridge != NULL && bridge->magic == PXSYS_ESP_PXA_MAGIC;
}

static lv_result_t schedule_on_lvgl_owner(lv_async_cb_t callback, void* context) {
    lv_result_t result;
    /* lv_async_call() creates an LVGL timer and therefore mutates LVGL's timer
     * list. PXA host callbacks arrive on the separate pxa_runtime task. */
    lv_lock();
    result = lv_async_call(callback, context);
    lv_unlock();
    return result;
}

static uint32_t advance_window_generation(pxsys_esp_pxa_bridge_t* bridge) {
    ++bridge->window_generation;
    if (bridge->window_generation == 0) ++bridge->window_generation;
    return bridge->window_generation;
}

static void begin_window_session(pxsys_esp_pxa_bridge_t* bridge) {
    if (!bridge_valid(bridge)) return;
    portENTER_CRITICAL(&g_bridge_lock);
    advance_window_generation(bridge);
    bridge->window_accepting = 1;
    portEXIT_CRITICAL(&g_bridge_lock);
}

static void resume_window_session(pxsys_esp_pxa_bridge_t* bridge) {
    if (!bridge_valid(bridge)) return;
    portENTER_CRITICAL(&g_bridge_lock);
    bridge->window_accepting = 1;
    portEXIT_CRITICAL(&g_bridge_lock);
}

static void reset_window(pxsys_esp_pxa_bridge_t* bridge) {
    pxsys_window_snapshot_t window;
    if (!bridge_valid(bridge)) return;
    /* Invalidate queued guest updates before publishing the desktop defaults.
     * A full-screen update can otherwise arrive after an app is backgrounded
     * and make the home screen full-screen again. */
    portENTER_CRITICAL(&g_bridge_lock);
    bridge->window_accepting = 0;
    advance_window_generation(bridge);
    portEXIT_CRITICAL(&g_bridge_lock);
    pxsys_window_snapshot_init(&window);
    (void)pxsys_window_service_update(
        pxsys_standard_system_window(bridge->system), &window);
}

static void apply_window_on_owner(void* context) {
    window_change_t* change = (window_change_t*)context;
    pxsys_esp_pxa_bridge_t* bridge = g_bridge;
    pxsys_window_snapshot_t window;
    int current = 0;
    if (change == NULL) return;
    portENTER_CRITICAL(&g_bridge_lock);
    if (bridge_valid(bridge) && bridge->generation == change->generation &&
        bridge->window_accepting &&
        bridge->window_generation == change->window_generation)
        current = 1;
    portEXIT_CRITICAL(&g_bridge_lock);
    if (!current) {
        free(change);
        return;
    }
    pxsys_window_snapshot_init(&window);
    window.edge_to_edge = change->configuration.edge_to_edge;
    window.status_bar_mode =
        (pxsys_window_bar_mode_t)change->configuration.status_bar_mode;
    window.navigation_bar_mode =
        (pxsys_window_bar_mode_t)change->configuration.navigation_bar_mode;
    window.status_bar_icons =
        (pxsys_window_icon_style_t)change->configuration.status_bar_icons;
    window.navigation_bar_icons =
        (pxsys_window_icon_style_t)change->configuration.navigation_bar_icons;
    window.status_bar_color = change->configuration.status_bar_color;
    window.navigation_bar_color = change->configuration.navigation_bar_color;
    (void)pxsys_window_service_update(
        pxsys_standard_system_window(bridge->system), &window);
    free(change);
}

static void window_changed(
    void* context, const pxa_window_configuration_t* configuration) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    window_change_t* change;
    uint32_t generation = 0;
    uint32_t window_generation = 0;
    if (configuration == NULL) return;
    change = (window_change_t*)malloc(sizeof(*change));
    if (change == NULL) return;
    portENTER_CRITICAL(&g_bridge_lock);
    if (g_bridge == bridge && bridge_valid(bridge) &&
        bridge->window_accepting) {
        generation = bridge->generation;
        window_generation = advance_window_generation(bridge);
    }
    portEXIT_CRITICAL(&g_bridge_lock);
    if (generation == 0) {
        free(change);
        return;
    }
    change->generation = generation;
    change->window_generation = window_generation;
    change->configuration = *configuration;
    if (schedule_on_lvgl_owner(apply_window_on_owner, change) != LV_RESULT_OK)
        free(change);
}

static esp_pxa_instance_t* find_instance(pxsys_esp_pxa_bridge_t* bridge,
                                         const char* identity_key) {
    esp_pxa_instance_t* instance;
    if (!bridge_valid(bridge) || identity_key == NULL)
        return NULL;
    for (instance = bridge->instances; instance != NULL;
         instance = instance->next) {
        if (strcmp(instance->identity_key, identity_key) == 0)
            return instance;
    }
    return NULL;
}

static void write_u16(uint8_t* output, uint16_t value) {
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static uint8_t* encode_intent_event(pxsys_esp_pxa_bridge_t* bridge,
                                    const pxsys_message_t* message,
                                    size_t* encoded_size) {
    const pxsys_caller_t* caller;
    size_t capacity;
    size_t offset = 0;
    uint8_t* encoded;
    if (encoded_size == NULL)
        return NULL;
    *encoded_size = 0;
    if (!bridge_valid(bridge) || message == NULL ||
        message->payload.data == NULL || message->payload.size == 0 ||
        message->payload.size > UINT16_MAX) {
        return NULL;
    }
    caller = message->caller;
    capacity = 4u + message->payload.size;
    if (caller != NULL) {
        if (caller->app.app_id.size == 0 || caller->app.app_id.size > UINT16_MAX ||
            caller->component_id.size == 0 || caller->component_id.size > UINT16_MAX)
            return NULL;
        capacity += 4u + PXSYS_PUBLISHER_ROOT_BYTES + 4u +
                    caller->app.app_id.size + 4u + caller->component_id.size;
    }
    encoded = (uint8_t*)bridge->allocator.allocate(bridge->allocator.context,
                                                   capacity);
    if (encoded == NULL)
        return NULL;
    write_u16(encoded + offset, 1);
    write_u16(encoded + offset + 2, (uint16_t)message->payload.size);
    memcpy(encoded + offset + 4, message->payload.data, message->payload.size);
    offset += 4u + message->payload.size;
    if (caller != NULL) {
        write_u16(encoded + offset, UINT16_C(0x8002));
        write_u16(encoded + offset + 2, PXSYS_PUBLISHER_ROOT_BYTES);
        memcpy(encoded + offset + 4, caller->app.publisher_root,
               PXSYS_PUBLISHER_ROOT_BYTES);
        offset += 4u + PXSYS_PUBLISHER_ROOT_BYTES;
        write_u16(encoded + offset, UINT16_C(0x8003));
        write_u16(encoded + offset + 2, (uint16_t)caller->app.app_id.size);
        memcpy(encoded + offset + 4, caller->app.app_id.data,
               caller->app.app_id.size);
        offset += 4u + caller->app.app_id.size;
        write_u16(encoded + offset, UINT16_C(0x8004));
        write_u16(encoded + offset + 2, (uint16_t)caller->component_id.size);
        memcpy(encoded + offset + 4, caller->component_id.data,
               caller->component_id.size);
        offset += 4u + caller->component_id.size;
    }
    *encoded_size = offset;
    return encoded;
}

static int identity_equal(const tracked_app_t* tracked, const pxa_host_package_info_t* package) {
    return package->has_publisher_root &&
           memcmp(tracked->identity.publisher_root, package->publisher_root,
                  PXSYS_PUBLISHER_ROOT_BYTES) == 0 &&
           strcmp(tracked->app_id, package->app_id) == 0;
}

static int package_in_snapshot(const tracked_app_t* tracked,
                               const pxa_host_package_info_t* packages, size_t count) {
    size_t index;
    for (index = 0; index < count; ++index) {
        if (packages[index].installed && identity_equal(tracked, &packages[index]))
            return 1;
    }
    return 0;
}

static int tracked_contains(const tracked_app_t* apps, size_t count,
                            const tracked_app_t* candidate) {
    size_t index;
    for (index = 0; index < count; ++index) {
        if (pxsys_app_identity_equal(&apps[index].identity, &candidate->identity))
            return 1;
    }
    return 0;
}

static void track_package(tracked_app_t* target, const pxa_host_package_info_t* package) {
    memset(target, 0, sizeof(*target));
    memcpy(target->identity.publisher_root, package->publisher_root, PXSYS_PUBLISHER_ROOT_BYTES);
    snprintf(target->app_id, sizeof(target->app_id), "%s", package->app_id);
    snprintf(target->identity_key, sizeof(target->identity_key), "%s",
             package->id);
    snprintf(target->resource_namespace, sizeof(target->resource_namespace),
             "app.%s", package->app_id);
    target->identity.app_id = pxsys_string_from_cstr(target->app_id);
}

static void copy_tracked(tracked_app_t* target, const tracked_app_t* source) {
    *target = *source;
    target->identity.app_id = pxsys_string_from_cstr(target->app_id);
}

static void resolve_tracked_metadata(pxsys_esp_pxa_bridge_t* bridge,
                                     size_t index, const char* locale) {
    tracked_app_t* tracked;
    if (!bridge_valid(bridge) || index >= bridge->tracked_count) return;
    tracked = &bridge->tracked[index];
    bridge->metadata_valid[index] = pxa_host_resolve_package_metadata(
        tracked->identity.publisher_root, tracked->app_id, locale,
        &bridge->metadata_cache[index]);
}

static size_t find_tracked_app(
    pxsys_esp_pxa_bridge_t* bridge, const pxsys_app_descriptor_t* app) {
    size_t index;
    if (!bridge_valid(bridge) || app == NULL) return SIZE_MAX;
    for (index = 0; index < bridge->tracked_count; ++index) {
        if (pxsys_app_identity_equal(&bridge->tracked[index].identity,
                                     &app->identity)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static void refresh_tracked_metadata(pxsys_esp_pxa_bridge_t* bridge,
                                     const char* locale) {
    size_t index;
    if (!bridge_valid(bridge) || locale == NULL) return;
    for (index = 0; index < bridge->tracked_count; ++index)
        resolve_tracked_metadata(bridge, index, locale);
    snprintf(bridge->metadata_locale, sizeof(bridge->metadata_locale), "%s",
             locale);
}

pxsys_status_t pxsys_esp_pxa_bridge_sync(pxsys_esp_pxa_bridge_t* bridge) {
    pxsys_app_registry_t* apps;
    size_t available;
    size_t package_count;
    size_t next_count = 0;
    size_t index;
    pxsys_locale_snapshot_t locale = {0};
    if (!bridge_valid(bridge))
        return PXSYS_STATUS_INVALID_ARGUMENT;
    apps = pxsys_standard_system_apps(bridge->system);
    locale.struct_size = sizeof(locale);
    if (pxsys_locale_service_get(
            pxsys_standard_system_locale(bridge->system), &locale) !=
        PXSYS_STATUS_OK) {
        return PXSYS_STATUS_BAD_STATE;
    }
    if (!pxa_host_set_locale(locale.tag, (uint8_t)locale.direction))
        return PXSYS_STATUS_UNAVAILABLE;
    available = pxa_host_package_count();
    package_count = pxa_host_list_packages(bridge->packages, bridge->capacity);
    for (index = 0; index < bridge->tracked_count; ++index) {
        if (!package_in_snapshot(&bridge->tracked[index], bridge->packages, package_count)) {
            pxsys_status_t status =
                pxsys_app_registry_unregister(apps, &bridge->tracked[index].identity);
            if (status == PXSYS_STATUS_BUSY && next_count < bridge->capacity)
                copy_tracked(&bridge->next[next_count++], &bridge->tracked[index]);
        }
    }
    for (index = 0; index < package_count && next_count < bridge->capacity; ++index) {
        const pxa_host_package_info_t* package = &bridge->packages[index];
        pxsys_app_descriptor_t descriptor = {0};
        pxsys_status_t status;
        tracked_app_t tracked;
        if (!package->installed || !package->has_publisher_root || package->id[0] == '\0' ||
            package->name[0] == '\0' || package->version[0] == '\0') {
            continue;
        }
        track_package(&tracked, package);
        descriptor.struct_size = sizeof(descriptor);
        descriptor.identity = tracked.identity;
        descriptor.display_name = pxsys_string_from_cstr(package->name);
        descriptor.resource_namespace =
            pxsys_string_from_cstr(tracked.resource_namespace);
        descriptor.display_name_resource_key =
            pxsys_string_from_cstr(PXSYS_APP_METADATA_NAME_KEY);
        descriptor.description_resource_key =
            pxsys_string_from_cstr(PXSYS_APP_METADATA_DESCRIPTION_KEY);
        descriptor.icon_resource_key =
            pxsys_string_from_cstr(PXSYS_APP_METADATA_ICON_KEY);
        descriptor.version = pxsys_string_from_cstr(package->version);
        descriptor.runtime_id = pxsys_string_from_cstr(PXSYS_ESP_PXA_RUNTIME_ID);
        descriptor.flags = PXSYS_APP_FLAG_SINGLE_INSTANCE |
                           PXSYS_APP_FLAG_LAUNCHER;
        if (!package->built_in)
            descriptor.flags |= PXSYS_APP_FLAG_REMOVABLE;
        if (package->enabled)
            descriptor.flags |= PXSYS_APP_FLAG_ENABLED;
        status = pxsys_app_registry_find(apps, &descriptor.identity) == NULL
                     ? pxsys_app_registry_register(apps, &descriptor)
                     : pxsys_app_registry_update(apps, &descriptor);
        if ((status == PXSYS_STATUS_OK || status == PXSYS_STATUS_BUSY) &&
            !tracked_contains(bridge->next, next_count, &tracked)) {
            copy_tracked(&bridge->next[next_count++], &tracked);
        }
    }
    for (index = 0; index < next_count; ++index)
        copy_tracked(&bridge->tracked[index], &bridge->next[index]);
    bridge->tracked_count = next_count;
    refresh_tracked_metadata(bridge, locale.tag);
    return available > bridge->capacity ? PXSYS_STATUS_RESOURCE_LIMIT : PXSYS_STATUS_OK;
}

bool pxsys_esp_pxa_bridge_resolve_app_metadata(
    pxsys_esp_pxa_bridge_t* bridge, const pxsys_app_descriptor_t* app,
    const pxsys_locale_snapshot_t* locale, pxsys_app_metadata_t* metadata) {
    size_t index;
    if (!bridge_valid(bridge) || app == NULL || locale == NULL ||
        metadata == NULL || app->identity.app_id.data == NULL) {
        return false;
    }
    if (strcmp(bridge->metadata_locale, locale->tag) != 0)
        refresh_tracked_metadata(bridge, locale->tag);
    index = find_tracked_app(bridge, app);
    if (index == SIZE_MAX || !bridge->metadata_valid[index]) return false;
    memset(metadata, 0, sizeof(*metadata));
    metadata->struct_size = sizeof(*metadata);
    metadata->display_name =
        pxsys_string_from_cstr(bridge->metadata_cache[index].name);
    metadata->description =
        pxsys_string_from_cstr(bridge->metadata_cache[index].description);
    metadata->icon_reference =
        pxsys_string_from_cstr(bridge->metadata_cache[index].icon_path);
    return true;
}

bool pxsys_esp_pxa_bridge_resolve_app_icon(
    pxsys_esp_pxa_bridge_t* bridge, const pxsys_app_descriptor_t* app,
    pxa_host_icon_t* icon) {
    size_t index;
    if (!bridge_valid(bridge) || app == NULL || icon == NULL) return false;
    index = find_tracked_app(bridge, app);
    if (index == SIZE_MAX || !bridge->metadata_valid[index]) return false;
    return pxa_host_load_package_icon_path(
        bridge->tracked[index].identity.publisher_root,
        bridge->tracked[index].app_id,
        bridge->metadata_cache[index].icon_path, icon);
}

static void sync_on_owner(void* context) {
    (void)context;
    if (bridge_valid(g_bridge)) {
        pxsys_esp_pxa_bridge_t* bridge = g_bridge;
        (void)pxsys_esp_pxa_bridge_sync(bridge);
        if (bridge_valid(bridge) && bridge->catalog_synced != NULL)
            bridge->catalog_synced(bridge->catalog_synced_context);
    }
}

static void catalog_changed(void* context) {
    (void)context;
    (void)schedule_on_lvgl_owner(sync_on_owner, NULL);
}

static void theme_changed(void* context, const pxsys_theme_snapshot_t* snapshot) {
    (void)context;
    if (snapshot == NULL)
        return;
    (void)pxa_host_set_color_scheme(
        snapshot->effective_scheme == PXSYS_COLOR_SCHEME_DARK
            ? PXA_HOST_COLOR_SCHEME_DARK
            : PXA_HOST_COLOR_SCHEME_LIGHT);
}

static int post_locale_to_instance(
    const esp_pxa_instance_t* instance,
    const pxsys_locale_snapshot_t* snapshot) {
    uint8_t payload[4u + PXSYS_LOCALE_TAG_MAX_BYTES + 4u + 1u];
    size_t offset = 0;
    if (instance == NULL || snapshot == NULL) return 0;
    write_u16(payload + offset, 1u);
    write_u16(payload + offset + 2u, snapshot->tag_size);
    memcpy(payload + offset + 4u, snapshot->tag, snapshot->tag_size);
    offset += 4u + snapshot->tag_size;
    write_u16(payload + offset, UINT16_C(0x8002));
    write_u16(payload + offset + 2u, 1u);
    payload[offset + 4u] = (uint8_t)snapshot->direction;
    offset += 5u;
    return pxa_host_post_app_system_event(
        instance->identity_key, PXA_HOST_SYSTEM_CONFIGURATION_EVENT,
        payload, offset);
}

static void locale_changed(void* context,
                           const pxsys_locale_snapshot_t* snapshot) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    esp_pxa_instance_t* instance;
    if (bridge == NULL || snapshot == NULL) return;
    (void)pxa_host_set_locale(snapshot->tag, (uint8_t)snapshot->direction);
    refresh_tracked_metadata(bridge, snapshot->tag);
    for (instance = bridge->instances; instance != NULL;
         instance = instance->next) {
        (void)post_locale_to_instance(instance, snapshot);
    }
}

static void process_runtime_event(void* context) {
    runtime_event_t* event = (runtime_event_t*)context;
    pxsys_esp_pxa_bridge_t* bridge = g_bridge;
    esp_pxa_instance_t* instance = NULL;
    if (event == NULL)
        return;
    if (bridge_valid(bridge) && bridge->generation == event->generation)
        instance = find_instance(bridge, event->identity);
    if (instance != NULL) {
        if (event->event == PXA_HOST_RUNTIME_STARTED) {
            uint8_t* launch_event = instance->launch_event;
            size_t launch_event_size = instance->launch_event_size;
            pxsys_locale_snapshot_t locale = {0};
            locale.struct_size = sizeof(locale);
            if (pxsys_locale_service_get(
                    pxsys_standard_system_locale(bridge->system),
                    &locale) == PXSYS_STATUS_OK) {
                (void)post_locale_to_instance(instance, &locale);
            }
            pxsys_status_t launch_status =
                launch_event != NULL &&
                        pxa_host_post_app_system_event(
                            instance->identity_key,
                            PXA_HOST_SYSTEM_INTENT_EVENT,
                            launch_event, launch_event_size)
                    ? PXSYS_STATUS_OK
                    : PXSYS_STATUS_UNAVAILABLE;
            instance->launch_event = NULL;
            instance->launch_event_size = 0;
            pxsys_status_t status = pxsys_task_manager_complete_start(
                pxsys_standard_system_tasks(bridge->system), instance->reference,
                launch_status);
            if (status == PXSYS_STATUS_NOT_FOUND) {
                (void)pxsys_role_host_complete_start(
                    pxsys_standard_system_role_host(bridge->system),
                    instance->reference, launch_status);
            }
            bridge->allocator.release(bridge->allocator.context, launch_event);
        } else if (event->event == PXA_HOST_RUNTIME_START_FAILED) {
            pxsys_status_t status = pxsys_task_manager_complete_start(
                pxsys_standard_system_tasks(bridge->system), instance->reference,
                PXSYS_STATUS_INTERNAL);
            if (status == PXSYS_STATUS_NOT_FOUND) {
                (void)pxsys_role_host_complete_start(
                    pxsys_standard_system_role_host(bridge->system),
                    instance->reference, PXSYS_STATUS_INTERNAL);
            }
        } else if (event->event == PXA_HOST_RUNTIME_STOPPED) {
            pxsys_status_t status = pxsys_task_manager_report_stopped(
                pxsys_standard_system_tasks(bridge->system), instance->reference,
                PXSYS_STOP_NORMAL);
            if (status == PXSYS_STATUS_NOT_FOUND) {
                (void)pxsys_role_host_report_stopped(
                    pxsys_standard_system_role_host(bridge->system),
                    instance->reference, PXSYS_STOP_NORMAL);
            }
        }
    }
    free(event);
}

static void runtime_event(void* context, pxa_host_runtime_event_t type, const char* identity) {
    runtime_event_t* event;
    uint32_t generation = 0;
    if (identity == NULL)
        return;
    portENTER_CRITICAL(&g_bridge_lock);
    if (g_bridge == context)
        generation = g_bridge->generation;
    portEXIT_CRITICAL(&g_bridge_lock);
    if (generation == 0)
        return;
    event = (runtime_event_t*)malloc(sizeof(*event));
    if (event == NULL)
        return;
    memset(event, 0, sizeof(*event));
    event->generation = generation;
    event->event = type;
    snprintf(event->identity, sizeof(event->identity), "%s", identity);
    if (schedule_on_lvgl_owner(process_runtime_event, event) != LV_RESULT_OK)
        free(event);
}

static void complete_system_request(system_request_t* request, pxsys_status_t status,
                                    const void* payload, size_t payload_size) {
    (void)pxa_host_complete_system_request(
        request->caller.app_id, request->component, request->request_id,
        (int32_t)pxsys_status_to_pxa(status), payload, payload_size);
}

static void complete_service_request(void* context, uint64_t request_id, pxsys_status_t status,
                                     pxsys_bytes_t payload) {
    system_request_t* request = (system_request_t*)context;
    if (request == NULL || request->completed || request_id != request->request_id)
        return;
    request->completed = 1;
    complete_system_request(request, status, payload.data, payload.size);
    if (request->invoke_returned)
        free(request);
}

static void init_caller(const system_request_t* request, pxsys_caller_t* caller) {
    memset(caller, 0, sizeof(*caller));
    caller->struct_size = sizeof(*caller);
    memcpy(caller->app.publisher_root, request->caller.publisher_root,
           sizeof(caller->app.publisher_root));
    caller->app.app_id = pxsys_string_from_cstr(request->caller.app_id);
    caller->component_id = pxsys_string_from_cstr(request->caller.component_id);
}

static void subscription_caller(const pxa_subscription_t* subscription, pxsys_caller_t* caller) {
    memset(caller, 0, sizeof(*caller));
    caller->struct_size = sizeof(*caller);
    memcpy(caller->app.publisher_root, subscription->caller.publisher_root,
           sizeof(caller->app.publisher_root));
    caller->app.app_id = pxsys_string_from_cstr(subscription->caller.app_id);
    caller->component_id = pxsys_string_from_cstr(subscription->caller.component_id);
}

static void deliver_topic_event(void* context, const pxsys_topic_event_t* event) {
    pxa_subscription_t* subscription = (pxa_subscription_t*)context;
    pxsys_esp_pxa_bridge_t* bridge;
    uint8_t* encoded;
    size_t capacity;
    size_t encoded_size;
    if (subscription == NULL || !subscription->occupied || event == NULL)
        return;
    bridge = subscription->bridge;
    if (!bridge_valid(bridge) || bridge->generation != subscription->generation ||
        event->payload.size > SIZE_MAX - 36u ||
        event->topic.size > SIZE_MAX - 36u - event->payload.size) {
        return;
    }
    capacity = event->topic.size + event->payload.size + 36u;
    if (capacity > PXA_MAX_CONTROL_MESSAGE - 12u)
        return;
    encoded = (uint8_t*)bridge->allocator.allocate(bridge->allocator.context, capacity);
    if (encoded == NULL)
        return;
    if (pxsys_pxa_gateway_encode_topic(event, encoded, capacity, &encoded_size) ==
        PXSYS_STATUS_OK) {
        (void)pxa_host_post_system_event(subscription->caller.app_id,
                                         subscription->caller.runtime_component,
                                         PXA_HOST_SYSTEM_TOPIC_EVENT, 0, encoded, encoded_size);
    }
    bridge->allocator.release(bridge->allocator.context, encoded);
}

static pxa_subscription_t* find_subscription(pxsys_esp_pxa_bridge_t* bridge,
                                             const pxa_host_system_caller_t* caller,
                                             pxsys_string_t topic) {
    size_t index;
    for (index = 0; index < bridge->subscription_capacity; ++index) {
        pxa_subscription_t* subscription = &bridge->subscriptions[index];
        if (subscription->occupied &&
            memcmp(subscription->caller.publisher_root, caller->publisher_root,
                   sizeof(caller->publisher_root)) == 0 &&
            strcmp(subscription->caller.app_id, caller->app_id) == 0 &&
            strcmp(subscription->caller.component_id, caller->component_id) == 0 &&
            strlen(subscription->topic) == topic.size &&
            memcmp(subscription->topic, topic.data, topic.size) == 0) {
            return subscription;
        }
    }
    return NULL;
}

static pxsys_status_t subscribe_topic(pxsys_esp_pxa_bridge_t* bridge, system_request_t* request,
                                      const pxsys_caller_t* caller, pxsys_topic_event_t* event) {
    pxa_subscription_t* subscription = NULL;
    pxsys_status_t status;
    size_t index;
    if (find_subscription(bridge, &request->caller, event->topic) != NULL)
        return PXSYS_STATUS_ALREADY_EXISTS;
    for (index = 0; index < bridge->subscription_capacity; ++index) {
        if (!bridge->subscriptions[index].occupied) {
            subscription = &bridge->subscriptions[index];
            break;
        }
    }
    if (subscription == NULL)
        return PXSYS_STATUS_RESOURCE_LIMIT;
    subscription->topic =
        (char*)bridge->allocator.allocate(bridge->allocator.context, event->topic.size + 1u);
    if (subscription->topic == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    memcpy(subscription->topic, event->topic.data, event->topic.size);
    subscription->topic[event->topic.size] = '\0';
    subscription->bridge = bridge;
    subscription->generation = bridge->generation;
    subscription->caller = request->caller;
    subscription->minimum_version = event->version;
    status = pxsys_event_subscribe(pxsys_standard_system_events(bridge->system), caller,
                                   event->topic, event->version, subscription, deliver_topic_event);
    if (status != PXSYS_STATUS_OK) {
        bridge->allocator.release(bridge->allocator.context, subscription->topic);
        memset(subscription, 0, sizeof(*subscription));
        return status;
    }
    subscription->occupied = 1;
    return PXSYS_STATUS_OK;
}

static pxsys_status_t unsubscribe_topic(pxsys_esp_pxa_bridge_t* bridge, system_request_t* request,
                                        const pxsys_caller_t* caller,
                                        const pxsys_topic_event_t* event) {
    pxa_subscription_t* subscription = find_subscription(bridge, &request->caller, event->topic);
    pxsys_status_t status;
    if (subscription == NULL)
        return PXSYS_STATUS_NOT_FOUND;
    status = pxsys_event_unsubscribe(pxsys_standard_system_events(bridge->system), caller,
                                     event->topic, subscription, deliver_topic_event);
    if (status == PXSYS_STATUS_OK) {
        bridge->allocator.release(bridge->allocator.context, subscription->topic);
        memset(subscription, 0, sizeof(*subscription));
    }
    return status;
}

static void remove_component_subscriptions(pxsys_esp_pxa_bridge_t* bridge,
                                           const esp_pxa_instance_t* instance) {
    size_t index;
    for (index = 0; index < bridge->subscription_capacity; ++index) {
        pxa_subscription_t* subscription = &bridge->subscriptions[index];
        pxsys_caller_t caller;
        if (!subscription->occupied ||
            memcmp(subscription->caller.publisher_root, instance->publisher_root,
                   sizeof(instance->publisher_root)) != 0 ||
            strcmp(subscription->caller.app_id, instance->app_id) != 0) {
            continue;
        }
        subscription_caller(subscription, &caller);
        (void)pxsys_event_unsubscribe(pxsys_standard_system_events(bridge->system), &caller,
                                      pxsys_string_from_cstr(subscription->topic), subscription,
                                      deliver_topic_event);
        bridge->allocator.release(bridge->allocator.context, subscription->topic);
        memset(subscription, 0, sizeof(*subscription));
    }
}

static pxa_endpoint_t* find_endpoint(pxsys_esp_pxa_bridge_t* bridge,
                                     const pxa_host_system_caller_t* caller,
                                     pxsys_string_t interface_id, uint16_t major) {
    size_t index;
    for (index = 0; index < bridge->endpoint_capacity; ++index) {
        pxa_endpoint_t* endpoint = &bridge->endpoints[index];
        if (endpoint->occupied && endpoint->version.major == major &&
            memcmp(endpoint->caller.publisher_root, caller->publisher_root,
                   sizeof(caller->publisher_root)) == 0 &&
            strcmp(endpoint->caller.app_id, caller->app_id) == 0 &&
            strcmp(endpoint->caller.component_id, caller->component_id) == 0 &&
            strlen(endpoint->interface_id) == interface_id.size &&
            memcmp(endpoint->interface_id, interface_id.data, interface_id.size) == 0) {
            return endpoint;
        }
    }
    return NULL;
}

static pxsys_status_t invoke_pxa_endpoint(void* context, const pxsys_service_request_t* request,
                                          void* completion_context,
                                          pxsys_service_complete_fn complete) {
    pxa_endpoint_t* endpoint = (pxa_endpoint_t*)context;
    pxsys_esp_pxa_bridge_t* bridge;
    pxa_pending_call_t* pending = NULL;
    uint8_t* encoded;
    size_t capacity = 88u;
    size_t encoded_size;
    size_t index;
    if (endpoint == NULL || !endpoint->occupied || request == NULL || complete == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    bridge = endpoint->bridge;
    if (!bridge_valid(bridge) || bridge->generation != endpoint->generation)
        return PXSYS_STATUS_UNAVAILABLE;
    for (index = 0; index < bridge->pending_call_capacity; ++index) {
        if (!bridge->pending_calls[index].occupied) {
            pending = &bridge->pending_calls[index];
            break;
        }
    }
    if (pending == NULL)
        return PXSYS_STATUS_RESOURCE_LIMIT;
#define ADD_WIRE_SIZE(value)                    \
    do {                                        \
        if ((value) > SIZE_MAX - capacity)      \
            return PXSYS_STATUS_RESOURCE_LIMIT; \
        capacity += (value);                    \
    } while (0)
    ADD_WIRE_SIZE(request->interface_id.size);
    ADD_WIRE_SIZE(request->payload.size);
    ADD_WIRE_SIZE(request->caller->app.app_id.size);
    ADD_WIRE_SIZE(request->caller->component_id.size);
#undef ADD_WIRE_SIZE
    if (capacity > PXA_MAX_CONTROL_MESSAGE - 12u)
        return PXSYS_STATUS_RESOURCE_LIMIT;
    encoded = (uint8_t*)bridge->allocator.allocate(bridge->allocator.context, capacity);
    if (encoded == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    bridge->next_call_id++;
    if (bridge->next_call_id == 0)
        bridge->next_call_id++;
    if (pxsys_pxa_gateway_encode_service_request(request, bridge->next_call_id, encoded, capacity,
                                                 &encoded_size) != PXSYS_STATUS_OK) {
        bridge->allocator.release(bridge->allocator.context, encoded);
        return PXSYS_STATUS_RESOURCE_LIMIT;
    }
    pending->endpoint = endpoint;
    pending->completion_context = completion_context;
    pending->complete = complete;
    pending->call_id = bridge->next_call_id;
    pending->request_id = request->request_id;
    pending->occupied = 1;
    if (!pxa_host_post_system_event(endpoint->caller.app_id, endpoint->caller.runtime_component,
                                    PXA_HOST_SYSTEM_SERVICE_REQUEST, 0, encoded, encoded_size)) {
        memset(pending, 0, sizeof(*pending));
        bridge->allocator.release(bridge->allocator.context, encoded);
        return PXSYS_STATUS_UNAVAILABLE;
    }
    bridge->allocator.release(bridge->allocator.context, encoded);
    return PXSYS_STATUS_OK;
}

static void cancel_endpoint_calls(pxsys_esp_pxa_bridge_t* bridge, pxa_endpoint_t* endpoint) {
    size_t index;
    for (index = 0; index < bridge->pending_call_capacity; ++index) {
        pxa_pending_call_t* pending = &bridge->pending_calls[index];
        pxsys_service_complete_fn complete;
        void* completion_context;
        uint64_t request_id;
        if (!pending->occupied || pending->endpoint != endpoint)
            continue;
        complete = pending->complete;
        completion_context = pending->completion_context;
        request_id = pending->request_id;
        memset(pending, 0, sizeof(*pending));
        complete(completion_context, request_id, PXSYS_STATUS_CANCELLED, pxsys_bytes(NULL, 0));
    }
}

static pxsys_status_t register_endpoint(pxsys_esp_pxa_bridge_t* bridge, system_request_t* request,
                                        const pxsys_pxa_service_descriptor_t* descriptor) {
    pxa_endpoint_t* endpoint = NULL;
    pxsys_service_provider_t provider = {0};
    pxsys_status_t status;
    size_t index;
    if (find_endpoint(bridge, &request->caller, descriptor->interface_id,
                      descriptor->version.major) != NULL) {
        return PXSYS_STATUS_ALREADY_EXISTS;
    }
    for (index = 0; index < bridge->endpoint_capacity; ++index) {
        if (!bridge->endpoints[index].occupied) {
            endpoint = &bridge->endpoints[index];
            break;
        }
    }
    if (endpoint == NULL)
        return PXSYS_STATUS_RESOURCE_LIMIT;
    endpoint->interface_id = (char*)bridge->allocator.allocate(bridge->allocator.context,
                                                               descriptor->interface_id.size + 1u);
    if (endpoint->interface_id == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    memcpy(endpoint->interface_id, descriptor->interface_id.data, descriptor->interface_id.size);
    endpoint->interface_id[descriptor->interface_id.size] = '\0';
    endpoint->bridge = bridge;
    endpoint->generation = bridge->generation;
    endpoint->caller = request->caller;
    endpoint->version = descriptor->version;
    endpoint->features = descriptor->features;
    provider.struct_size = sizeof(provider);
    provider.interface_id = pxsys_string_from_cstr(endpoint->interface_id);
    provider.version = endpoint->version;
    provider.features = endpoint->features;
    provider.context = endpoint;
    provider.invoke = invoke_pxa_endpoint;
    status = pxsys_service_register(pxsys_standard_system_services(bridge->system), &provider);
    if (status != PXSYS_STATUS_OK) {
        bridge->allocator.release(bridge->allocator.context, endpoint->interface_id);
        memset(endpoint, 0, sizeof(*endpoint));
        return status;
    }
    endpoint->occupied = 1;
    return PXSYS_STATUS_OK;
}

static pxsys_status_t unregister_endpoint(pxsys_esp_pxa_bridge_t* bridge, system_request_t* request,
                                          const pxsys_pxa_service_descriptor_t* descriptor) {
    pxa_endpoint_t* endpoint = find_endpoint(bridge, &request->caller, descriptor->interface_id,
                                             descriptor->version.major);
    pxsys_status_t status;
    if (endpoint == NULL)
        return PXSYS_STATUS_NOT_FOUND;
    cancel_endpoint_calls(bridge, endpoint);
    status = pxsys_service_unregister(pxsys_standard_system_services(bridge->system),
                                      descriptor->interface_id, descriptor->version.major);
    if (status == PXSYS_STATUS_OK) {
        bridge->allocator.release(bridge->allocator.context, endpoint->interface_id);
        memset(endpoint, 0, sizeof(*endpoint));
    }
    return status;
}

static pxsys_status_t finish_endpoint_call(pxsys_esp_pxa_bridge_t* bridge,
                                           system_request_t* request,
                                           const pxsys_pxa_service_completion_t* completion) {
    size_t index;
    for (index = 0; index < bridge->pending_call_capacity; ++index) {
        pxa_pending_call_t* pending = &bridge->pending_calls[index];
        pxsys_service_complete_fn complete;
        void* completion_context;
        uint64_t request_id;
        if (!pending->occupied || pending->call_id != completion->call_id ||
            memcmp(pending->endpoint->caller.publisher_root, request->caller.publisher_root,
                   sizeof(request->caller.publisher_root)) != 0 ||
            strcmp(pending->endpoint->caller.app_id, request->caller.app_id) != 0 ||
            strcmp(pending->endpoint->caller.component_id, request->caller.component_id) != 0) {
            continue;
        }
        complete = pending->complete;
        completion_context = pending->completion_context;
        request_id = pending->request_id;
        memset(pending, 0, sizeof(*pending));
        complete(completion_context, request_id, pxsys_status_from_pxa(completion->status),
                 completion->payload);
        return PXSYS_STATUS_OK;
    }
    return PXSYS_STATUS_NOT_FOUND;
}

static void remove_app_endpoints(pxsys_esp_pxa_bridge_t* bridge,
                                 const esp_pxa_instance_t* instance) {
    size_t index;
    for (index = 0; index < bridge->endpoint_capacity; ++index) {
        pxa_endpoint_t* endpoint = &bridge->endpoints[index];
        if (!endpoint->occupied ||
            memcmp(endpoint->caller.publisher_root, instance->publisher_root,
                   sizeof(instance->publisher_root)) != 0 ||
            strcmp(endpoint->caller.app_id, instance->app_id) != 0) {
            continue;
        }
        cancel_endpoint_calls(bridge, endpoint);
        (void)pxsys_service_unregister(pxsys_standard_system_services(bridge->system),
                                       pxsys_string_from_cstr(endpoint->interface_id),
                                       endpoint->version.major);
        bridge->allocator.release(bridge->allocator.context, endpoint->interface_id);
        memset(endpoint, 0, sizeof(*endpoint));
    }
}

static void process_system_request(void* context) {
    system_request_t* request = (system_request_t*)context;
    pxsys_esp_pxa_bridge_t* bridge = g_bridge;
    pxsys_status_t status = PXSYS_STATUS_UNAVAILABLE;
    pxsys_caller_t caller;
    if (request == NULL)
        return;
    if (!bridge_valid(bridge) || bridge->generation != request->generation) {
        complete_system_request(request, status, NULL, 0);
        free(request);
        return;
    }
    init_caller(request, &caller);
    if (request->opcode == PXA_HOST_SYSTEM_INTENT_START) {
        pxsys_intent_t intent;
        pxsys_app_identity_t target;
        pxsys_instance_ref_t instance;
        status =
            pxsys_intent_wire_decode(request->payload, request->payload_size, &intent, &target);
        if (status == PXSYS_STATUS_OK) {
            status = pxsys_task_manager_start_as(pxsys_standard_system_tasks(bridge->system),
                                                 &caller, &intent, &instance);
            if (status == PXSYS_STATUS_PENDING)
                status = PXSYS_STATUS_OK;
        }
    } else if (request->opcode == PXA_HOST_SYSTEM_SERVICE_INVOKE) {
        pxsys_service_request_t service;
        status = pxsys_pxa_gateway_decode_service(
            (pxa_bytes_t){request->payload, request->payload_size}, &service);
        if (status == PXSYS_STATUS_OK) {
            service.request_id = request->request_id;
            service.caller = &caller;
            status = pxsys_service_invoke(pxsys_standard_system_services(bridge->system), &service,
                                          request, complete_service_request);
            request->invoke_returned = 1;
            if (request->completed) {
                free(request);
                return;
            }
            if (status == PXSYS_STATUS_OK)
                return;
        }
    } else if (request->opcode == PXA_HOST_SYSTEM_TOPIC_PUBLISH) {
        pxsys_topic_event_t event;
        size_t delivered = 0;
        status = pxsys_pxa_gateway_decode_topic(
            (pxa_bytes_t){request->payload, request->payload_size}, &event);
        if (status == PXSYS_STATUS_OK) {
            uint8_t response[4];
            event.publisher = &caller;
            status = pxsys_event_publish(pxsys_standard_system_events(bridge->system), &event,
                                         &delivered);
            if (status == PXSYS_STATUS_OK) {
                pxa_write_u32(response, (uint32_t)delivered);
                complete_system_request(request, status, response, sizeof(response));
                free(request);
                return;
            }
        }
    } else if (request->opcode == PXA_HOST_SYSTEM_TOPIC_SUBSCRIBE ||
               request->opcode == PXA_HOST_SYSTEM_TOPIC_UNSUBSCRIBE) {
        pxsys_topic_event_t event;
        status = pxsys_pxa_gateway_decode_topic(
            (pxa_bytes_t){request->payload, request->payload_size}, &event);
        if (status == PXSYS_STATUS_OK) {
            status = request->opcode == PXA_HOST_SYSTEM_TOPIC_SUBSCRIBE
                         ? subscribe_topic(bridge, request, &caller, &event)
                         : unsubscribe_topic(bridge, request, &caller, &event);
        }
    } else if (request->opcode == PXA_HOST_SYSTEM_SERVICE_REGISTER ||
               request->opcode == PXA_HOST_SYSTEM_SERVICE_UNREGISTER) {
        pxsys_pxa_service_descriptor_t descriptor;
        status = pxsys_pxa_gateway_decode_service_descriptor(
            (pxa_bytes_t){request->payload, request->payload_size}, &descriptor);
        if (status == PXSYS_STATUS_OK) {
            status = request->opcode == PXA_HOST_SYSTEM_SERVICE_REGISTER
                         ? register_endpoint(bridge, request, &descriptor)
                         : unregister_endpoint(bridge, request, &descriptor);
        }
    } else if (request->opcode == PXA_HOST_SYSTEM_SERVICE_COMPLETE) {
        pxsys_pxa_service_completion_t completion;
        status = pxsys_pxa_gateway_decode_service_completion(
            (pxa_bytes_t){request->payload, request->payload_size}, &completion);
        if (status == PXSYS_STATUS_OK)
            status = finish_endpoint_call(bridge, request, &completion);
    } else {
        status = PXSYS_STATUS_UNSUPPORTED;
    }
    complete_system_request(request, status, NULL, 0);
    free(request);
}

static bool system_request(void* context, const pxa_host_system_caller_t* caller, uint16_t opcode,
                           uint32_t request_id, const uint8_t* payload, size_t payload_size) {
    system_request_t* request;
    uint32_t generation = 0;
    if (caller == NULL || request_id == 0 || (payload == NULL && payload_size != 0) ||
        payload_size > SIZE_MAX - sizeof(*request)) {
        return false;
    }
    portENTER_CRITICAL(&g_bridge_lock);
    if (g_bridge == context)
        generation = g_bridge->generation;
    portEXIT_CRITICAL(&g_bridge_lock);
    if (generation == 0)
        return false;
    request = malloc(sizeof(*request) + payload_size);
    if (request == NULL)
        return false;
    memset(request, 0, sizeof(*request));
    request->generation = generation;
    request->component = caller->runtime_component;
    request->request_id = request_id;
    request->opcode = opcode;
    request->payload_size = payload_size;
    request->caller = *caller;
    if (payload_size != 0)
        memcpy(request->payload, payload, payload_size);
    if (schedule_on_lvgl_owner(process_system_request, request) != LV_RESULT_OK) {
        free(request);
        return false;
    }
    return true;
}

static pxsys_status_t backend_instantiate(void* context, const pxsys_app_descriptor_t* app,
                                          uint64_t instance_id, void** output) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    esp_pxa_instance_t* instance;
    (void)instance_id;
    if (!bridge_valid(bridge) || app == NULL || output == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    *output = NULL;
    if (app->identity.app_id.size >= PXA_HOST_APP_ID_MAX)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    instance = (esp_pxa_instance_t*)bridge->allocator.allocate(bridge->allocator.context,
                                                               sizeof(*instance));
    if (instance == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    memset(instance, 0, sizeof(*instance));
    instance->bridge = bridge;
    memcpy(instance->publisher_root, app->identity.publisher_root,
           sizeof(instance->publisher_root));
    memcpy(instance->app_id, app->identity.app_id.data, app->identity.app_id.size);
    instance->app_id[app->identity.app_id.size] = '\0';
    for (size_t index = 0; index < bridge->tracked_count; ++index) {
        if (pxsys_app_identity_equal(&bridge->tracked[index].identity,
                                     &app->identity)) {
            snprintf(instance->identity_key, sizeof(instance->identity_key),
                     "%s", bridge->tracked[index].identity_key);
            break;
        }
    }
    if (instance->identity_key[0] == '\0') {
        bridge->allocator.release(bridge->allocator.context, instance);
        return PXSYS_STATUS_NOT_FOUND;
    }
    instance->reference = pxsys_instance_ref_invalid();
    instance->next = bridge->instances;
    bridge->instances = instance;
    *output = instance;
    return PXSYS_STATUS_OK;
}

static void backend_bound(void* context, void* backend_instance, pxsys_instance_ref_t reference) {
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    (void)context;
    if (instance != NULL)
        instance->reference = reference;
}

static pxsys_status_t backend_start(void* context, void* backend_instance,
                                    const pxsys_message_t* launch) {
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    if (instance == NULL || instance->launch_event != NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    instance->launch_event =
        encode_intent_event(bridge, launch, &instance->launch_event_size);
    if (instance->launch_event == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    /* Guest startup can configure its window before the runtime reports that
     * it is ready to foreground. Accept that initial configuration as part of
     * this launch's window session. */
    begin_window_session(bridge);
    if (pxa_host_runtime_launch(instance->identity_key))
        return PXSYS_STATUS_PENDING;
    reset_window(bridge);
    bridge->allocator.release(bridge->allocator.context,
                              instance->launch_event);
    instance->launch_event = NULL;
    instance->launch_event_size = 0;
    return PXSYS_STATUS_UNAVAILABLE;
}

static pxsys_status_t backend_foreground(void* context, void* backend_instance) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    if (backend_instance == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    resume_window_session(bridge);
    pxa_esp_surface_set_host_visible(true);
    return PXSYS_STATUS_OK;
}

static pxsys_status_t backend_background(void* context, void* backend_instance) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    if (backend_instance == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    pxa_esp_surface_set_host_visible(false);
    reset_window(bridge);
    return PXSYS_STATUS_OK;
}

static pxsys_status_t backend_deliver(void* context, void* backend_instance,
                                      const pxsys_message_t* message) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    uint8_t* encoded;
    size_t encoded_size;
    int posted;
    if (instance == NULL || message == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    encoded = encode_intent_event(bridge, message, &encoded_size);
    if (encoded == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    posted = pxa_host_post_app_system_event(
        instance->identity_key, PXA_HOST_SYSTEM_INTENT_EVENT, encoded,
        encoded_size);
    bridge->allocator.release(bridge->allocator.context, encoded);
    return posted ? PXSYS_STATUS_OK : PXSYS_STATUS_UNAVAILABLE;
}

static pxsys_back_result_t backend_back(void* context, void* backend_instance) {
    (void)context;
    return backend_instance != NULL && pxa_host_runtime_back() ? PXSYS_BACK_HANDLED
                                                               : PXSYS_BACK_UNHANDLED;
}

static void backend_stop(void* context, void* backend_instance, pxsys_stop_reason_t reason) {
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    (void)reason;
    if (instance != NULL) {
        pxa_esp_surface_set_host_visible(false);
        (void)pxa_host_runtime_stop(instance->identity_key);
    }
    reset_window(bridge);
}

static pxsys_status_t backend_request_stop(void* context, void* backend_instance,
                                           pxsys_stop_reason_t reason) {
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    (void)reason;
    if (instance == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    pxa_esp_surface_set_host_visible(false);
    reset_window(bridge);
    return pxa_host_runtime_stop(instance->identity_key) ? PXSYS_STATUS_PENDING
                                                     : PXSYS_STATUS_UNAVAILABLE;
}

static void backend_destroy(void* context, void* backend_instance) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    esp_pxa_instance_t* instance = (esp_pxa_instance_t*)backend_instance;
    esp_pxa_instance_t** cursor;
    if (!bridge_valid(bridge) || instance == NULL)
        return;
    remove_component_subscriptions(bridge, instance);
    remove_app_endpoints(bridge, instance);
    bridge->allocator.release(bridge->allocator.context,
                              instance->launch_event);
    for (cursor = &bridge->instances; *cursor != NULL;
         cursor = &(*cursor)->next) {
        if (*cursor != instance)
            continue;
        *cursor = instance->next;
        break;
    }
    bridge->allocator.release(bridge->allocator.context, instance);
}

/* A launch request may arrive on any task: the standalone UI asks for it on
 * the LVGL owner, PXADB asks for it on its own service task. The task manager
 * backgrounds the running app and rebuilds trusted UI synchronously, so the
 * whole start must run on the LVGL owner. Queue it there and return
 * immediately: the PXADB response is "launch_queued" and the PXA runtime
 * reports the final start state through the normal runtime events. */
typedef struct {
    pxsys_esp_pxa_bridge_t* bridge;
    uint32_t generation;
    pxsys_app_identity_t target;
    char app_id[PXA_HOST_APP_ID_MAX];
} pending_launch_t;

static void perform_launch_request(void* context) {
    pending_launch_t* request = (pending_launch_t*)context;
    pxsys_intent_t intent = {0};
    pxsys_instance_ref_t instance;
    pxsys_status_t status = PXSYS_STATUS_INTERNAL;
    if (request == NULL) return;
    if (bridge_valid(request->bridge) &&
        request->bridge->generation == request->generation) {
        intent.struct_size = sizeof(intent);
        intent.action = pxsys_string_from_cstr("system.intent.main");
        intent.target = &request->target;
        status = pxsys_task_manager_start(
            pxsys_standard_system_tasks(request->bridge->system), &intent,
            &instance);
    }
    if (status != PXSYS_STATUS_OK && status != PXSYS_STATUS_PENDING) {
        ESP_LOGW(PXSYS_ESP_PXA_TAG, "Launch request failed: %s status=%d",
                 request->app_id, (int)status);
    }
    free(request);
}

static bool launch_requested(void* context,
                             const uint8_t publisher_root[PXA_HOST_PUBLISHER_ROOT_BYTES],
                             const char* identity) {
    pxsys_esp_pxa_bridge_t* bridge = (pxsys_esp_pxa_bridge_t*)context;
    pending_launch_t* request;
    size_t index;
    if (!bridge_valid(bridge) || identity == NULL)
        return false;
    for (index = 0; index < bridge->tracked_count; ++index) {
        if (memcmp(bridge->tracked[index].identity.publisher_root, publisher_root,
                   PXSYS_PUBLISHER_ROOT_BYTES) != 0 ||
            strcmp(bridge->tracked[index].app_id, identity) != 0)
            continue;
        request = (pending_launch_t*)calloc(1, sizeof(*request));
        if (request == NULL) return false;
        request->bridge = bridge;
        request->generation = bridge->generation;
        request->target = bridge->tracked[index].identity;
        snprintf(request->app_id, sizeof(request->app_id), "%s",
                 bridge->tracked[index].app_id);
        request->target.app_id = pxsys_string_from_cstr(request->app_id);
        if (schedule_on_lvgl_owner(perform_launch_request, request) !=
            LV_RESULT_OK) {
            free(request);
            return false;
        }
        return true;
    }
    return false;
}

void pxsys_esp_pxa_bridge_config_init(pxsys_esp_pxa_bridge_config_t* config) {
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->max_packages = 64;
    config->max_subscriptions = 64;
    config->max_exported_services = 16;
    config->max_pending_calls = 16;
    config->allocator.struct_size = sizeof(config->allocator);
}

pxsys_status_t pxsys_esp_pxa_bridge_create(const pxsys_esp_pxa_bridge_config_t* config,
                                           pxsys_esp_pxa_bridge_t** output) {
    pxsys_esp_pxa_bridge_t* bridge;
    pxsys_pxa_runtime_config_t runtime = {0};
    pxsys_runtime_provider_t provider;
    pxsys_status_t status;
    size_t tracked_bytes;
    if (output == NULL)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    *output = NULL;
    if (config == NULL || config->struct_size < sizeof(*config) || config->system == NULL ||
        config->max_packages == 0 || config->max_subscriptions == 0 ||
        config->max_exported_services == 0 || config->max_pending_calls == 0 ||
        config->max_packages > SIZE_MAX / sizeof(tracked_app_t) ||
        config->max_packages > SIZE_MAX / sizeof(pxa_host_package_metadata_t) ||
        config->max_packages > SIZE_MAX / sizeof(pxa_host_package_info_t) ||
        config->max_subscriptions > SIZE_MAX / sizeof(pxa_subscription_t) ||
        config->max_exported_services > SIZE_MAX / sizeof(pxa_endpoint_t) ||
        config->max_pending_calls > SIZE_MAX / sizeof(pxa_pending_call_t) ||
        config->allocator.struct_size < sizeof(config->allocator) ||
        config->allocator.allocate == NULL || config->allocator.release == NULL ||
        g_bridge != NULL) {
        return PXSYS_STATUS_INVALID_ARGUMENT;
    }
    bridge = (pxsys_esp_pxa_bridge_t*)config->allocator.allocate(config->allocator.context,
                                                                 sizeof(*bridge));
    if (bridge == NULL)
        return PXSYS_STATUS_NO_MEMORY;
    memset(bridge, 0, sizeof(*bridge));
    bridge->allocator = config->allocator;
    bridge->system = config->system;
    bridge->catalog_synced_context = config->catalog_synced_context;
    bridge->catalog_synced = config->catalog_synced;
    bridge->capacity = config->max_packages;
    bridge->subscription_capacity = config->max_subscriptions;
    bridge->endpoint_capacity = config->max_exported_services;
    bridge->pending_call_capacity = config->max_pending_calls;
    bridge->generation = ++g_generation;
    if (bridge->generation == 0)
        bridge->generation = ++g_generation;
    tracked_bytes = bridge->capacity * sizeof(*bridge->tracked);
    bridge->tracked =
        (tracked_app_t*)bridge->allocator.allocate(bridge->allocator.context, tracked_bytes);
    bridge->next =
        (tracked_app_t*)bridge->allocator.allocate(bridge->allocator.context, tracked_bytes);
    bridge->metadata_cache =
        (pxa_host_package_metadata_t*)bridge->allocator.allocate(
            bridge->allocator.context,
            bridge->capacity * sizeof(*bridge->metadata_cache));
    bridge->metadata_valid = (uint8_t*)bridge->allocator.allocate(
        bridge->allocator.context,
        bridge->capacity * sizeof(*bridge->metadata_valid));
    bridge->packages = (pxa_host_package_info_t*)bridge->allocator.allocate(
        bridge->allocator.context, bridge->capacity * sizeof(*bridge->packages));
    bridge->subscriptions = (pxa_subscription_t*)bridge->allocator.allocate(
        bridge->allocator.context, bridge->subscription_capacity * sizeof(*bridge->subscriptions));
    bridge->endpoints = (pxa_endpoint_t*)bridge->allocator.allocate(
        bridge->allocator.context, bridge->endpoint_capacity * sizeof(*bridge->endpoints));
    bridge->pending_calls = (pxa_pending_call_t*)bridge->allocator.allocate(
        bridge->allocator.context, bridge->pending_call_capacity * sizeof(*bridge->pending_calls));
    if (bridge->tracked == NULL || bridge->next == NULL ||
        bridge->metadata_cache == NULL || bridge->metadata_valid == NULL ||
        bridge->packages == NULL ||
        bridge->subscriptions == NULL || bridge->endpoints == NULL ||
        bridge->pending_calls == NULL) {
        status = PXSYS_STATUS_NO_MEMORY;
        goto failed;
    }
    memset(bridge->tracked, 0, tracked_bytes);
    memset(bridge->next, 0, tracked_bytes);
    memset(bridge->metadata_cache, 0,
           bridge->capacity * sizeof(*bridge->metadata_cache));
    memset(bridge->metadata_valid, 0,
           bridge->capacity * sizeof(*bridge->metadata_valid));
    memset(bridge->packages, 0, bridge->capacity * sizeof(*bridge->packages));
    memset(bridge->subscriptions, 0,
           bridge->subscription_capacity * sizeof(*bridge->subscriptions));
    memset(bridge->endpoints, 0, bridge->endpoint_capacity * sizeof(*bridge->endpoints));
    memset(bridge->pending_calls, 0,
           bridge->pending_call_capacity * sizeof(*bridge->pending_calls));
    status = pxsys_theme_service_subscribe(pxsys_standard_system_theme(bridge->system), bridge,
                                           theme_changed);
    if (status != PXSYS_STATUS_OK)
        goto failed;
    bridge->theme_subscribed = 1;
    status = pxsys_locale_service_subscribe(
        pxsys_standard_system_locale(bridge->system), bridge, locale_changed);
    if (status != PXSYS_STATUS_OK)
        goto failed;
    bridge->locale_subscribed = 1;
    runtime.struct_size = sizeof(runtime);
    runtime.runtime_id = pxsys_string_from_cstr(PXSYS_ESP_PXA_RUNTIME_ID);
    runtime.version = (pxsys_version_t){0, 1};
    runtime.allocator = bridge->allocator;
    runtime.backend.struct_size = sizeof(runtime.backend);
    runtime.backend.context = bridge;
    runtime.backend.instantiate = backend_instantiate;
    runtime.backend.bound = backend_bound;
    runtime.backend.start = backend_start;
    runtime.backend.foreground = backend_foreground;
    runtime.backend.background = backend_background;
    runtime.backend.deliver = backend_deliver;
    runtime.backend.back = backend_back;
    runtime.backend.stop = backend_stop;
    runtime.backend.destroy = backend_destroy;
    runtime.backend.request_stop = backend_request_stop;
    status = pxsys_pxa_runtime_create(&runtime, &bridge->pxa_runtime);
    if (status != PXSYS_STATUS_OK)
        goto failed;
    status = pxsys_pxa_runtime_provider(bridge->pxa_runtime, &provider);
    if (status != PXSYS_STATUS_OK)
        goto failed;
    status =
        pxsys_runtime_register_provider(pxsys_standard_system_runtime(bridge->system), &provider);
    if (status != PXSYS_STATUS_OK)
        goto failed;
    bridge->magic = PXSYS_ESP_PXA_MAGIC;
    portENTER_CRITICAL(&g_bridge_lock);
    g_bridge = bridge;
    portEXIT_CRITICAL(&g_bridge_lock);
    pxa_host_set_runtime_event_callback(runtime_event, bridge);
    pxa_host_set_catalog_changed_callback(catalog_changed, bridge);
    pxa_host_set_launch_request_callback(launch_requested, bridge);
    pxa_host_set_system_request_callback(system_request, bridge);
    pxa_host_set_window_changed_callback(window_changed, bridge);
    (void)pxsys_esp_pxa_bridge_sync(bridge);
    *output = bridge;
    return PXSYS_STATUS_OK;

failed:
    if (bridge->locale_subscribed) {
        (void)pxsys_locale_service_unsubscribe(
            pxsys_standard_system_locale(bridge->system), bridge,
            locale_changed);
    }
    if (bridge->theme_subscribed) {
        (void)pxsys_theme_service_unsubscribe(pxsys_standard_system_theme(bridge->system), bridge,
                                              theme_changed);
    }
    if (bridge->pxa_runtime != NULL)
        (void)pxsys_pxa_runtime_destroy(bridge->pxa_runtime);
    if (bridge->subscriptions != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->subscriptions);
    if (bridge->endpoints != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->endpoints);
    if (bridge->pending_calls != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->pending_calls);
    if (bridge->packages != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->packages);
    if (bridge->metadata_valid != NULL)
        bridge->allocator.release(bridge->allocator.context,
                                  bridge->metadata_valid);
    if (bridge->metadata_cache != NULL)
        bridge->allocator.release(bridge->allocator.context,
                                  bridge->metadata_cache);
    if (bridge->next != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->next);
    if (bridge->tracked != NULL)
        bridge->allocator.release(bridge->allocator.context, bridge->tracked);
    bridge->allocator.release(bridge->allocator.context, bridge);
    return status;
}

pxsys_status_t pxsys_esp_pxa_bridge_destroy(pxsys_esp_pxa_bridge_t* bridge) {
    pxsys_runtime_provider_t provider;
    pxsys_status_t status;
    pxsys_status_t rollback_status;
    size_t index;
    int provider_unregistered = 0;
    if (!bridge_valid(bridge) || g_bridge != bridge)
        return PXSYS_STATUS_INVALID_ARGUMENT;
    status = pxsys_theme_service_unsubscribe(pxsys_standard_system_theme(bridge->system), bridge,
                                             theme_changed);
    if (status != PXSYS_STATUS_OK)
        return status;
    bridge->theme_subscribed = 0;
    status = pxsys_locale_service_unsubscribe(
        pxsys_standard_system_locale(bridge->system), bridge, locale_changed);
    if (status != PXSYS_STATUS_OK)
        goto restore_callbacks;
    bridge->locale_subscribed = 0;
    pxa_host_set_launch_request_callback(NULL, NULL);
    pxa_host_set_catalog_changed_callback(NULL, NULL);
    pxa_host_set_runtime_event_callback(NULL, NULL);
    pxa_host_set_system_request_callback(NULL, NULL);
    pxa_host_set_window_changed_callback(NULL, NULL);
    (void)lv_async_call_cancel(sync_on_owner, NULL);
    status = pxsys_task_manager_finish_all(pxsys_standard_system_tasks(bridge->system),
                                           PXSYS_STOP_SHUTDOWN);
    if (status != PXSYS_STATUS_OK)
        goto restore_callbacks;
    status = pxsys_runtime_unregister_provider(pxsys_standard_system_runtime(bridge->system),
                                               pxsys_string_from_cstr(PXSYS_ESP_PXA_RUNTIME_ID));
    if (status != PXSYS_STATUS_OK)
        goto restore_callbacks;
    provider_unregistered = 1;
    status = pxsys_pxa_runtime_destroy(bridge->pxa_runtime);
    if (status != PXSYS_STATUS_OK)
        goto restore_callbacks;
    for (index = 0; index < bridge->tracked_count; ++index)
        (void)pxsys_app_registry_unregister(pxsys_standard_system_apps(bridge->system),
                                            &bridge->tracked[index].identity);
    portENTER_CRITICAL(&g_bridge_lock);
    g_bridge = NULL;
    portEXIT_CRITICAL(&g_bridge_lock);
    bridge->magic = 0;
    bridge->allocator.release(bridge->allocator.context, bridge->pending_calls);
    bridge->allocator.release(bridge->allocator.context, bridge->endpoints);
    bridge->allocator.release(bridge->allocator.context, bridge->subscriptions);
    bridge->allocator.release(bridge->allocator.context, bridge->packages);
    bridge->allocator.release(bridge->allocator.context,
                              bridge->metadata_valid);
    bridge->allocator.release(bridge->allocator.context,
                              bridge->metadata_cache);
    bridge->allocator.release(bridge->allocator.context, bridge->next);
    bridge->allocator.release(bridge->allocator.context, bridge->tracked);
    bridge->allocator.release(bridge->allocator.context, bridge);
    return PXSYS_STATUS_OK;

restore_callbacks:
    if (provider_unregistered) {
        rollback_status = pxsys_pxa_runtime_provider(bridge->pxa_runtime, &provider);
        if (rollback_status == PXSYS_STATUS_OK) {
            rollback_status = pxsys_runtime_register_provider(
                pxsys_standard_system_runtime(bridge->system), &provider);
        }
        if (rollback_status != PXSYS_STATUS_OK)
            status = rollback_status;
    }
    if (!bridge->theme_subscribed &&
        pxsys_theme_service_subscribe(pxsys_standard_system_theme(bridge->system), bridge,
                                      theme_changed) == PXSYS_STATUS_OK)
        bridge->theme_subscribed = 1;
    if (!bridge->locale_subscribed &&
        pxsys_locale_service_subscribe(
            pxsys_standard_system_locale(bridge->system), bridge,
            locale_changed) == PXSYS_STATUS_OK)
        bridge->locale_subscribed = 1;
    pxa_host_set_runtime_event_callback(runtime_event, bridge);
    pxa_host_set_catalog_changed_callback(catalog_changed, bridge);
    pxa_host_set_launch_request_callback(launch_requested, bridge);
    pxa_host_set_system_request_callback(system_request, bridge);
    pxa_host_set_window_changed_callback(window_changed, bridge);
    return status;
}

#endif
