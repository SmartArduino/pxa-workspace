#include "pxa_esp_services.h"

#if defined(ESP_PLATFORM)

#include "sdkconfig.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "pxa_esp_audio.h"
#include "pxa/pxa_esp_surface.h"
#include "pxa_esp_permission_store.h"

#ifndef CONFIG_PXA_STATE_ROOT
#define CONFIG_PXA_STATE_ROOT "pxa-state"
#endif
#ifndef CONFIG_PXA_MOUNT_POINT
#define CONFIG_PXA_MOUNT_POINT "/assets"
#endif
#ifndef CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES
#define CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES (64 * 1024)
#endif
#ifndef CONFIG_PXA_PRIVATE_FS_MAX_OPEN_HANDLES
#define CONFIG_PXA_PRIVATE_FS_MAX_OPEN_HANDLES 8
#endif
#ifndef CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES
#define CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES 8192
#endif
#ifndef CONFIG_PXA_PRIVATE_KV_MAX_VALUE_BYTES
#define CONFIG_PXA_PRIVATE_KV_MAX_VALUE_BYTES 960
#endif

#define PXA_ESP_SERVICES_PATH_BYTES 160
#define PXA_ESP_GUEST_LOG_TAG "PXA-App"

static pxa_status_t write_guest_log(
    void *context, pxa_component_t component, pxa_bytes_t app_id,
    pxa_log_level_t level, pxa_bytes_t message) {
    static const char *const names[] = {
        "TRACE", "DEBUG", "INFO", "WARN", "ERROR",
    };
    static const char *const colors[] = {
        "\033[90m", "\033[36m", "\033[32m", "\033[33m", "\033[31m",
    };
    static const esp_log_level_t levels[] = {
        ESP_LOG_VERBOSE, ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_WARN,
        ESP_LOG_ERROR,
    };
    (void)context;
    if (level > PXA_LOG_LEVEL_ERROR) return PXA_STATUS_INVALID_ARGUMENT;
    esp_log_write(levels[level], PXA_ESP_GUEST_LOG_TAG,
                  "%s[PXA app=%.*s component=%lu level=%s] %.*s\033[0m\n",
                  colors[level], (int)app_id.size, (const char *)app_id.data,
                  (unsigned long)component, names[level], (int)message.size,
                  (const char *)message.data);
    return PXA_STATUS_OK;
}

static pxa_status_t fail(pxa_esp_services_result_t *result,
                         const char *stage, pxa_status_t status,
                         pxa_esp_services_issue_t issue) {
    result->stage = stage;
    result->issue = issue;
    return status;
}

static pxa_status_t allocate_workspace(
    const pxa_esp_services_config_t *config, size_t size, void **output,
    pxa_esp_services_result_t *result, const char *stage) {
    *output = size == 0 ? NULL : config->allocate(config->allocator_context,
                                                  size);
    return *output != NULL
               ? PXA_STATUS_OK
               : fail(result, stage, PXA_STATUS_RESOURCE_LIMIT,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static void *ui_allocate(void *context, size_t size) {
    void *memory;
    (void)context;
    memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return memory;
}

static void ui_release(void *context, void *memory) {
    (void)context;
    free(memory);
}

static uint64_t now_us(void *context) {
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

static uint64_t clock_ms(void *context) {
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static pxa_status_t empty_sensor_subscribe(void *context,
                                           uint16_t descriptor_id,
                                           uint32_t period_ms,
                                           void **subscription) {
    (void)context;
    (void)descriptor_id;
    (void)period_ms;
    if (subscription != NULL) *subscription = NULL;
    return PXA_STATUS_NOT_FOUND;
}

static pxa_status_t empty_sensor_read(
    void *context, void *subscription, uint16_t descriptor_id,
    int32_t values[PXA_SENSOR_MAX_DIMENSIONS]) {
    (void)context;
    (void)subscription;
    (void)descriptor_id;
    (void)values;
    return PXA_STATUS_NOT_FOUND;
}

static void empty_sensor_unsubscribe(void *context, void *subscription,
                                     uint16_t descriptor_id) {
    (void)context;
    (void)subscription;
    (void)descriptor_id;
}

static pxa_status_t esp_get_mac(void *context, uint16_t kind,
                                uint8_t output[6], uint32_t *flags) {
    (void)context;
    if (output == NULL || flags == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (kind != PXA_DEVICE_MAC_KIND_WIFI_STATION_HARDWARE) {
        return PXA_STATUS_NOT_FOUND;
    }
    if (esp_read_mac(output, ESP_MAC_WIFI_STA) != ESP_OK) {
        return PXA_STATUS_UNAVAILABLE;
    }
    *flags = PXA_DEVICE_MAC_FLAG_HARDWARE;
    return PXA_STATUS_OK;
}

static pxa_status_t collect_declarations(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    const pxa_package_manifest_t *manifest = host->manifest;
    uint16_t index;
    services->declaration_count = 0;
    for (index = 0; index < manifest->permission_count; ++index) {
        const pxa_package_permission_t *permission =
            &manifest->permissions[index];
        if (permission->name.data == NULL || permission->name.size == 0 ||
            permission->name.size > 96 ||
            (permission->scope.data == NULL && permission->scope.size != 0) ||
            permission->scope.size > 1024 || permission->required > 1) {
            return fail(result, "validate-permission-declarations",
                        PXA_STATUS_INVALID_ARGUMENT,
                        PXA_ESP_SERVICES_ISSUE_INVALID_PERMISSIONS);
        }
    }
    if (manifest->permission_count == 0) return PXA_STATUS_OK;
    services->declarations = host->allocate(
        host->allocator_context,
        (size_t)manifest->permission_count *
            sizeof(services->declarations[0]));
    if (services->declarations == NULL) {
        return fail(result, "allocate-permission-declarations",
                    PXA_STATUS_RESOURCE_LIMIT,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    for (index = 0; index < manifest->permission_count; ++index) {
        const pxa_package_permission_t *permission =
            &manifest->permissions[index];
        pxa_permission_declaration_t *declaration =
            &services->declarations[services->declaration_count++];
        declaration->name = permission->name;
        declaration->scope = permission->scope;
        declaration->required = permission->required;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t initialize_permission(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    pxa_permission_config_t config;
    pxa_permission_store_t store;
    pxa_status_t status;
    size_t workspace_size;
    status = collect_declarations(services, host, result);
    if (status != PXA_STATUS_OK) return status;
    pxa_esp_permission_store_bind(&store);
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.app_identity = (pxa_bytes_t){(const uint8_t *)host->identity,
                                        strlen(host->identity)};
    config.declarations = services->declarations;
    config.declaration_count = services->declaration_count;
    config.max_authorities = 4;
    config.max_pending_prompts = 1;
    config.prompt_context = host->permission_prompt_context;
    config.prompt = host->permission_prompt;
    config.store = store;
    workspace_size = pxa_permission_service_workspace_size(&config);
    status = allocate_workspace(host, workspace_size,
                                &services->permission_workspace, result,
                                "allocate-permission-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_permission_service_init(
        services->permission_workspace, workspace_size, host->runtime, &config,
        &services->permission);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-permission-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    status = pxa_permission_policy_load(services->permission);
    if (status != PXA_STATUS_OK) {
        return fail(result, "load-permission-policy", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    if (!pxa_permission_can_activate(services->permission)) {
        return fail(result, "authorize-required-permissions",
                    PXA_STATUS_DENIED,
                    PXA_ESP_SERVICES_ISSUE_REQUIRED_PERMISSION_DENIED);
    }
    return PXA_STATUS_OK;
}

static pxa_status_t initialize_fs(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    pxa_posix_fs_config_t config;
    pxa_fs_config_t service_config;
    char root[PXA_ESP_SERVICES_PATH_BYTES];
    pxa_status_t status;
    size_t workspace_size;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    if (snprintf(root, sizeof(root), "%s/%s/data/%s", CONFIG_PXA_MOUNT_POINT,
                 CONFIG_PXA_STATE_ROOT, host->identity) >= (int)sizeof(root) ||
        (mkdir(root, 0700) != 0 && errno != EEXIST)) {
        return fail(result, "create-private-fs-root", PXA_STATUS_INTERNAL,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    config.root_path = root;
    config.quota_bytes = CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES;
    config.max_open_resources = CONFIG_PXA_PRIVATE_FS_MAX_OPEN_HANDLES;
    workspace_size = pxa_posix_fs_workspace_size(&config);
    status = allocate_workspace(host, workspace_size,
                                &services->posix_fs_workspace, result,
                                "allocate-private-fs");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_posix_fs_init(services->posix_fs_workspace, workspace_size,
                               &config, &services->posix_fs,
                               &services->fs_backend);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-private-fs", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    memset(&service_config, 0, sizeof(service_config));
    service_config.struct_size = sizeof(service_config);
    service_config.max_open_resources =
        CONFIG_PXA_PRIVATE_FS_MAX_OPEN_HANDLES;
    service_config.backend = services->fs_backend;
    workspace_size = pxa_fs_service_workspace_size(&service_config);
    status = allocate_workspace(host, workspace_size, &services->fs_workspace,
                                result, "allocate-fs-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_fs_service_init(services->fs_workspace, workspace_size,
                                 host->runtime, &service_config,
                                 &services->fs);
    return status == PXA_STATUS_OK
               ? status
               : fail(result, "initialize-fs-service", status,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static pxa_status_t ensure_storage(pxa_esp_services_t *services) {
    size_t size;
    if (services->posix_storage != NULL) return PXA_STATUS_OK;
    size = pxa_posix_storage_workspace_size(&services->storage_config);
    if (services->posix_storage_workspace == NULL)
        services->posix_storage_workspace = services->storage_allocate(
            services->storage_allocator_context, size);
    if (services->posix_storage_workspace == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    return pxa_posix_storage_init(services->posix_storage_workspace, size,
        &services->storage_config, &services->posix_storage,
        &services->loaded_storage_backend);
}

static pxa_status_t lazy_storage_get(void *context, pxa_bytes_t key,
    uint8_t *output, size_t capacity, size_t *size) {
    pxa_esp_services_t *services = context;
    pxa_status_t status = ensure_storage(services);
    if (status != PXA_STATUS_OK) return status;
    return services->loaded_storage_backend.get(
        services->loaded_storage_backend.context, key, output, capacity, size);
}

static pxa_status_t lazy_storage_set(void *context, pxa_bytes_t key,
                                     pxa_bytes_t value) {
    pxa_esp_services_t *services = context;
    pxa_status_t status = ensure_storage(services);
    if (status != PXA_STATUS_OK) return status;
    return services->loaded_storage_backend.set(
        services->loaded_storage_backend.context, key, value);
}

static pxa_status_t lazy_storage_remove(void *context, pxa_bytes_t key) {
    pxa_esp_services_t *services = context;
    pxa_status_t status = ensure_storage(services);
    if (status != PXA_STATUS_OK) return status;
    return services->loaded_storage_backend.remove(
        services->loaded_storage_backend.context, key);
}

static pxa_status_t lazy_storage_list(void *context, pxa_bytes_t cursor,
    pxa_storage_emit_key_fn emit, void *emit_context) {
    pxa_esp_services_t *services = context;
    pxa_status_t status = ensure_storage(services);
    if (status != PXA_STATUS_OK) return status;
    return services->loaded_storage_backend.list(
        services->loaded_storage_backend.context, cursor, emit, emit_context);
}

static pxa_status_t initialize_storage(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    pxa_posix_storage_config_t config;
    pxa_storage_config_t service_config;
    char root[PXA_ESP_SERVICES_PATH_BYTES];
    pxa_status_t status;
    size_t workspace_size;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    if (snprintf(root, sizeof(root), "%s/%s/data/%s/.pxa-storage",
                 CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
                 host->identity) >= (int)sizeof(root) ||
        (mkdir(root, 0700) != 0 && errno != EEXIST)) {
        return fail(result, "create-private-storage-root", PXA_STATUS_INTERNAL,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    config.root_path = root;
    config.max_keys = 32;
    config.max_value_bytes = CONFIG_PXA_PRIVATE_KV_MAX_VALUE_BYTES;
    config.quota_bytes = CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES;
    {
        char *owned_root = host->allocate(host->allocator_context, strlen(root) + 1u);
        if (owned_root == NULL)
            return fail(result, "allocate-storage-root", PXA_STATUS_RESOURCE_LIMIT,
                        PXA_ESP_SERVICES_ISSUE_NONE);
        strcpy(owned_root, root);
        config.root_path = owned_root;
    }
    services->storage_config = config;
    services->storage_allocate = host->allocate;
    services->storage_allocator_context = host->allocator_context;
    services->storage_backend.struct_size = sizeof(services->storage_backend);
    services->storage_backend.context = services;
    services->storage_backend.get = lazy_storage_get;
    services->storage_backend.set = lazy_storage_set;
    services->storage_backend.remove = lazy_storage_remove;
    services->storage_backend.list = lazy_storage_list;
    memset(&service_config, 0, sizeof(service_config));
    service_config.struct_size = sizeof(service_config);
    service_config.max_value_bytes = CONFIG_PXA_PRIVATE_KV_MAX_VALUE_BYTES;
    service_config.backend = services->storage_backend;
    workspace_size = pxa_storage_service_workspace_size(&service_config);
    status = allocate_workspace(host, workspace_size,
                                &services->storage_workspace, result,
                                "allocate-storage-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_storage_service_init(
        services->storage_workspace, workspace_size, host->runtime,
        &service_config, &services->storage);
    return status == PXA_STATUS_OK
               ? status
               : fail(result, "initialize-storage-service", status,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static pxa_status_t initialize_bounded_services(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    pxa_ipc_limits_t ipc_limits;
    pxa_lease_limits_t lease_limits;
    pxa_log_config_t log_config;
    pxa_sensor_config_t sensor_config;
    pxa_device_config_t device_config;
    pxa_net_backend_t net_backend;
    pxa_net_config_t net_config;
    pxa_audio_backend_t audio_backend;
    pxa_audio_config_t audio_config;
    pxa_surface_backend_t surface_backend;
    pxa_surface_config_t surface_config;
    pxa_game_render_backend_t game_render_backend;
    pxa_game_render_config_t game_render_config;
    pxa_status_t status;
    size_t workspace_size;

    pxa_ipc_limits_init(&ipc_limits);
    ipc_limits.max_endpoints = 16;
    ipc_limits.max_pending_calls = 16;
    workspace_size = pxa_ipc_broker_workspace_size(&ipc_limits);
    status = allocate_workspace(host, workspace_size, &services->ipc_workspace,
                                result, "allocate-ipc-broker");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_ipc_broker_init(services->ipc_workspace, workspace_size,
                                 host->runtime, &ipc_limits, &services->ipc);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-ipc-broker", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    pxa_lease_limits_init(&lease_limits);
    lease_limits.allowed_kinds = (UINT32_C(1) << 6) - 1u;
    lease_limits.max_leases = 8;
    lease_limits.max_leases_per_component = 4;
    lease_limits.default_duration_ms = 5000;
    lease_limits.max_duration_ms = 30000;
    lease_limits.clock = clock_ms;
    workspace_size = pxa_lease_service_workspace_size(&lease_limits);
    status = allocate_workspace(host, workspace_size,
                                &services->lease_workspace, result,
                                "allocate-lease-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_lease_service_init(services->lease_workspace, workspace_size,
                                    host->runtime, &lease_limits,
                                    &services->lease);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-lease-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    memset(&log_config, 0, sizeof(log_config));
    log_config.struct_size = sizeof(log_config);
    log_config.write = write_guest_log;
    log_config.app_id = host->manifest->app_id;
    log_config.max_message_bytes = PXA_LOG_MAX_MESSAGE_BYTES;
    workspace_size = pxa_log_service_workspace_size(&log_config);
    status = allocate_workspace(host, workspace_size, &services->log_workspace,
                                result, "allocate-log-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_log_service_init(services->log_workspace, workspace_size,
                                  host->runtime, &log_config, &services->log);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-log-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    memset(&sensor_config, 0, sizeof(sensor_config));
    sensor_config.struct_size = sizeof(sensor_config);
    sensor_config.max_subscriptions = 8;
    sensor_config.max_subscriptions_per_component = 4;
    sensor_config.subscribe = empty_sensor_subscribe;
    sensor_config.read = empty_sensor_read;
    sensor_config.unsubscribe = empty_sensor_unsubscribe;
    sensor_config.permissions = services->permission;
    workspace_size = pxa_sensor_service_workspace_size(&sensor_config);
    status = allocate_workspace(host, workspace_size,
                                &services->sensor_workspace, result,
                                "allocate-sensor-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_sensor_service_init(
        services->sensor_workspace, workspace_size, host->runtime,
        &sensor_config, &services->sensor);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-sensor-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    memset(&device_config, 0, sizeof(device_config));
    device_config.struct_size = sizeof(device_config);
    device_config.get_mac = esp_get_mac;
    device_config.permissions = services->permission;
    workspace_size = pxa_device_service_workspace_size(&device_config);
    status = allocate_workspace(host, workspace_size, &services->device_workspace,
                                result, "allocate-device-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_device_service_init(
        services->device_workspace, workspace_size, host->runtime,
        &device_config, &services->device);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-device-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    if (!pxa_esp_net_backend(&net_backend, host->net_notify,
                             host->net_notify_context)) {
        return fail(result, "initialize-net-backend",
                    PXA_STATUS_RESOURCE_LIMIT,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    memset(&net_config, 0, sizeof(net_config));
    net_config.struct_size = sizeof(net_config);
    net_config.max_pending_requests = PXA_ESP_NET_MAX_PENDING;
    net_config.max_requests_per_component = 2;
    net_config.max_response_streams = PXA_ESP_NET_MAX_PENDING;
    net_config.max_response_bytes = PXA_ESP_NET_MAX_RESPONSE_BYTES;
    net_config.max_headers = PXA_NET_MAX_HEADERS;
    net_config.max_inline_body_bytes = PXA_ESP_NET_MAX_INLINE_BODY_BYTES;
    net_config.max_request_header_bytes = PXA_ESP_NET_REQUEST_HEADER_BYTES;
    net_config.max_response_header_bytes = PXA_ESP_NET_RESPONSE_HEADER_BYTES;
    net_config.min_timeout_ms = 100;
    net_config.default_timeout_ms = PXA_ESP_NET_DEFAULT_TIMEOUT_MS;
    net_config.max_timeout_ms = 60000;
    net_config.backend = net_backend;
    net_config.permissions = services->permission;
    workspace_size = pxa_net_service_workspace_size(&net_config);
    status = allocate_workspace(host, workspace_size, &services->net_workspace,
                                result, "allocate-net-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_net_service_init(services->net_workspace, workspace_size,
                                  host->runtime, &net_config, &services->net);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-net-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    pxa_esp_audio_backend(&audio_backend);
    memset(&audio_config, 0, sizeof(audio_config));
    audio_config.struct_size = sizeof(audio_config);
    audio_config.max_sessions = PXA_ESP_AUDIO_VOICE_COUNT;
    audio_config.max_sessions_per_component = PXA_ESP_AUDIO_VOICE_COUNT;
    audio_config.max_eq_bands = PXA_AUDIO_MAX_EQ_BANDS;
    audio_config.backend = audio_backend;
    audio_config.permissions = services->permission;
    workspace_size = pxa_audio_service_workspace_size(&audio_config);
    status = allocate_workspace(host, workspace_size,
                                &services->audio_workspace, result,
                                "allocate-audio-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_audio_service_init(
        services->audio_workspace, workspace_size, host->runtime,
        &audio_config, &services->audio);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-audio-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    pxa_esp_surface_backend(&surface_backend);
    memset(&surface_config, 0, sizeof(surface_config));
    surface_config.struct_size = sizeof(surface_config);
    surface_config.max_surfaces = 1;
    surface_config.max_surfaces_per_component = 1;
    surface_config.max_width = 320;
    surface_config.max_height = 240;
    surface_config.max_frame_bytes = 320u * 240u * 4u;
    surface_config.min_buffer_count = 2;
    surface_config.max_buffer_count = 3;
    surface_config.backend = surface_backend;
    workspace_size = pxa_surface_service_workspace_size(&surface_config);
    status = allocate_workspace(host, workspace_size,
                                &services->surface_workspace, result,
                                "allocate-surface-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_surface_service_init(
        services->surface_workspace, workspace_size, host->runtime,
        &surface_config, &services->surface);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-surface-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    pxa_esp_game_render_backend(&game_render_backend);
    memset(&game_render_config, 0, sizeof(game_render_config));
    game_render_config.struct_size = sizeof(game_render_config);
    game_render_config.max_contexts = 1;
    game_render_config.max_contexts_per_component = 1;
    game_render_config.max_width = 320;
    game_render_config.max_height = 240;
    game_render_config.min_buffer_count = 2;
    game_render_config.max_buffer_count = 3;
    game_render_config.backend = game_render_backend;
    workspace_size =
        pxa_game_render_service_workspace_size(&game_render_config);
    status = allocate_workspace(host, workspace_size,
                                &services->game_render_workspace, result,
                                "allocate-game-render-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_game_render_service_init(
        services->game_render_workspace, workspace_size, host->runtime,
        &game_render_config, &services->game_render);
    return status == PXA_STATUS_OK
               ? status
               : fail(result, "initialize-game-render-service", status,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static pxa_status_t collect_jobs(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    const pxa_package_manifest_t *manifest = host->manifest;
    uint16_t component_index;
    uint16_t count = 0;
    services->job_component_count = 0;
    for (component_index = 0; component_index < manifest->component_count;
         ++component_index) {
        const pxa_package_component_t *component =
            &manifest->components[component_index];
        if (component->kind != 3) continue;
        if (component->id.size == 0 ||
            component->id.size >= PXA_ESP_SERVICES_MAX_COMPONENT_ID_BYTES) {
            return fail(result, "validate-job-components",
                        PXA_STATUS_INVALID_ARGUMENT,
                        PXA_ESP_SERVICES_ISSUE_INVALID_JOB);
        }
        ++count;
    }
    if (count == 0) return PXA_STATUS_OK;
    services->job_components = host->allocate(
        host->allocator_context,
        (size_t)count * sizeof(services->job_components[0]));
    services->job_component_ids = host->allocate(
        host->allocator_context,
        (size_t)count * sizeof(services->job_component_ids[0]));
    if (services->job_components == NULL ||
        services->job_component_ids == NULL) {
        return fail(result, "allocate-job-components",
                    PXA_STATUS_RESOURCE_LIMIT, PXA_ESP_SERVICES_ISSUE_NONE);
    }
    memset(services->job_components, 0,
           (size_t)count * sizeof(services->job_components[0]));
    for (component_index = 0; component_index < manifest->component_count;
         ++component_index) {
        const pxa_package_component_t *component =
            &manifest->components[component_index];
        char *destination;
        if (component->kind != 3) continue;
        destination = services->job_components[services->job_component_count];
        memcpy(destination, component->id.data, component->id.size);
        destination[component->id.size] = '\0';
        services->job_component_ids[services->job_component_count] =
            (pxa_bytes_t){(const uint8_t *)destination, component->id.size};
        services->job_component_count++;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t empty_work_load(void *context, pxa_scheduler_entry_t *entries,
                                    size_t capacity, size_t *count) {
    (void)context;
    (void)entries;
    (void)capacity;
    *count = 0;
    return PXA_STATUS_OK;
}

static pxa_status_t empty_work_save(void *context,
    const pxa_scheduler_entry_t *entries, size_t count) {
    (void)context;
    (void)entries;
    return count == 0 ? PXA_STATUS_OK : PXA_STATUS_BAD_STATE;
}

static int storage_snapshots_absent(const pxa_esp_services_t *services) {
    char path[PXA_ESP_SERVICES_PATH_BYTES];
    struct stat metadata;
    const char *slots[] = {".pxa-kv-a", ".pxa-kv-b"};
    size_t index;
    for (index = 0; index < 2; ++index) {
        int length = snprintf(path, sizeof(path), "%s/%s",
                              services->storage_config.root_path, slots[index]);
        if (length < 0 || (size_t)length >= sizeof(path) ||
            stat(path, &metadata) == 0 || errno != ENOENT) return 0;
    }
    return 1;
}

static pxa_status_t initialize_scheduler(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    static const uint8_t store_key[] = "work.v1";
    pxa_scheduler_config_t config;
    pxa_posix_scheduler_store_config_t store_config;
    pxa_scheduler_store_t store;
    pxa_status_t status;
    size_t workspace_size;
    status = collect_jobs(services, host, result);
    if (status != PXA_STATUS_OK) return status;
    pxa_scheduler_config_init(&config);
    config.job_components =
        services->job_component_count == 0 ? NULL
                                           : services->job_component_ids;
    config.job_component_count = services->job_component_count;
    config.clock = clock_ms;
    config.work_context = host->work_context;
    config.complete_work = host->complete_work;
    config.cancel_work = host->cancel_work;
    if (services->job_component_count == 0 && storage_snapshots_absent(services)) {
        /* Existing snapshots still need validation to discard work belonging
         * to workers removed by a Package update. */
        memset(&store, 0, sizeof(store));
        store.struct_size = sizeof(store);
        store.load = empty_work_load;
        store.save = empty_work_save;
    } else {
        memset(&store_config, 0, sizeof(store_config));
        store_config.struct_size = sizeof(store_config);
        store_config.backend = services->storage_backend;
        store_config.key = (pxa_bytes_t){store_key, sizeof(store_key) - 1u};
        store_config.max_entries = config.max_entries;
        store_config.epoch = host->work_epoch;
        workspace_size =
            pxa_posix_scheduler_store_workspace_size(&store_config);
        status = allocate_workspace(host, workspace_size,
                                    &services->scheduler_store_workspace, result,
                                    "allocate-scheduler-store");
        if (status != PXA_STATUS_OK) return status;
        status = pxa_posix_scheduler_store_init(
            services->scheduler_store_workspace, workspace_size, &store_config,
            &services->scheduler_store, &store);
        if (status != PXA_STATUS_OK) {
            return fail(result, "initialize-scheduler-store", status,
                        PXA_ESP_SERVICES_ISSUE_NONE);
        }
    }
    config.store = store;
    workspace_size = pxa_scheduler_service_workspace_size(&config);
    status = allocate_workspace(host, workspace_size,
                                &services->scheduler_workspace, result,
                                "allocate-scheduler-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_scheduler_service_init(
        services->scheduler_workspace, workspace_size, host->runtime, &config,
        &services->scheduler);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-scheduler-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }
    status = pxa_scheduler_load(services->scheduler);
    if (status == PXA_STATUS_INTERNAL) {
        status = store.save(store.context, NULL, 0);
        if (status == PXA_STATUS_OK) {
            status = pxa_scheduler_load(services->scheduler);
        }
    }
    return status == PXA_STATUS_OK
               ? status
               : fail(result, "load-scheduler-state", status,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static pxa_status_t initialize_window_ui(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *host,
    pxa_esp_services_result_t *result) {
    pxa_ui_config_t config;
    pxa_status_t status;
    size_t workspace_size;
    workspace_size = pxa_window_service_workspace_size(8);
    status = allocate_workspace(host, workspace_size,
                                &services->window_workspace, result,
                                "allocate-window-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_window_service_init(services->window_workspace,
                                     workspace_size, host->runtime, 8,
                                     &services->window);
    if (status != PXA_STATUS_OK) {
        return fail(result, "initialize-window-service", status,
                    PXA_ESP_SERVICES_ISSUE_NONE);
    }

    pxa_ui_config_init(&config);
    config.allocate = ui_allocate;
    config.release = ui_release;
    config.now_us = now_us;
    config.max_dynamic_bytes = SIZE_MAX;
    config.max_transaction_bytes = SIZE_MAX;
    config.max_canvas_bytes = SIZE_MAX;
    config.features = PXA_UI_FEATURE_CANVAS | PXA_UI_FEATURE_VIRTUAL_LIST |
                      PXA_UI_FEATURE_RGB565_BITMAP |
                      PXA_UI_FEATURE_CONTROLLER_INPUT |
                      PXA_UI_FEATURE_CANVAS_STREAM_IO;
    config.primary_width = 320;
    config.primary_height = 240;
    config.color_scheme = host->color_scheme;
    workspace_size = pxa_ui_service_workspace_size();
    status = allocate_workspace(host, workspace_size, &services->ui_workspace,
                                result, "allocate-ui-service");
    if (status != PXA_STATUS_OK) return status;
    status = pxa_ui_service_init(services->ui_workspace, workspace_size,
                                 host->runtime, &config, &services->ui);
    return status == PXA_STATUS_OK
               ? status
               : fail(result, "initialize-ui-service", status,
                      PXA_ESP_SERVICES_ISSUE_NONE);
}

static pxa_status_t register_services(
    pxa_esp_services_t *services, pxa_esp_services_result_t *result) {
    pxa_status_t status;
#define PXA_REGISTER(call, stage_name)                                         \
    do {                                                                        \
        status = (call);                                                        \
        if (status != PXA_STATUS_OK) {                                          \
            return fail(result, (stage_name), status,                           \
                        PXA_ESP_SERVICES_ISSUE_NONE);                            \
        }                                                                       \
    } while (0)
    PXA_REGISTER(pxa_storage_service_register(services->storage),
                 "register-storage-service");
    PXA_REGISTER(pxa_fs_service_register(services->fs),
                 "register-fs-service");
    PXA_REGISTER(pxa_window_service_register(services->window),
                 "register-window-service");
    PXA_REGISTER(pxa_ui_service_register(services->ui), "register-ui-service");
    PXA_REGISTER(pxa_ipc_broker_register(services->ipc), "register-ipc-broker");
    PXA_REGISTER(pxa_lease_service_register(services->lease),
                 "register-lease-service");
    PXA_REGISTER(pxa_log_service_register(services->log),
                 "register-log-service");
    PXA_REGISTER(pxa_permission_service_register(services->permission),
                 "register-permission-service");
    PXA_REGISTER(pxa_scheduler_service_register(services->scheduler),
                 "register-scheduler-service");
    PXA_REGISTER(pxa_sensor_service_register(services->sensor),
                 "register-sensor-service");
    PXA_REGISTER(pxa_device_service_register(services->device),
                 "register-device-service");
    PXA_REGISTER(pxa_net_service_register(services->net),
                 "register-net-service");
    PXA_REGISTER(pxa_audio_service_register(services->audio),
                 "register-audio-service");
    PXA_REGISTER(pxa_surface_service_register(services->surface),
                 "register-surface-service");
    PXA_REGISTER(pxa_game_render_service_register(services->game_render),
                 "register-game-render-service");
#undef PXA_REGISTER
    return PXA_STATUS_OK;
}

pxa_status_t pxa_esp_services_initialize(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *config,
    pxa_esp_services_result_t *result) {
    pxa_status_t status;
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (services == NULL || config == NULL || result == NULL ||
        config->runtime == NULL || config->manifest == NULL ||
        config->identity == NULL || config->identity[0] == '\0' ||
        config->allocate == NULL || config->permission_prompt == NULL ||
        config->net_notify == NULL ||
        config->color_scheme > PXA_UI_COLOR_SCHEME_DARK) {
        if (result != NULL) {
            result->stage = "validate-service-config";
        }
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    pxa_esp_services_destroy(services);
    status = initialize_permission(services, config, result);
    if (status == PXA_STATUS_OK) status = initialize_fs(services, config, result);
    if (status == PXA_STATUS_OK) {
        status = initialize_storage(services, config, result);
    }
    if (status == PXA_STATUS_OK) {
        status = initialize_bounded_services(services, config, result);
    }
    if (status == PXA_STATUS_OK) {
        status = initialize_scheduler(services, config, result);
    }
    if (status == PXA_STATUS_OK) {
        status = initialize_window_ui(services, config, result);
    }
    if (status == PXA_STATUS_OK) status = register_services(services, result);
    return status;
}

void pxa_esp_services_destroy(pxa_esp_services_t *services) {
    if (services == NULL) return;
    if (services->ui != NULL) pxa_ui_service_deinit(services->ui);
    if (services->scheduler_store != NULL) {
        pxa_posix_scheduler_store_deinit(services->scheduler_store);
    }
    if (services->posix_storage != NULL) {
        pxa_posix_storage_deinit(services->posix_storage);
    }
    if (services->posix_fs != NULL) {
        pxa_posix_fs_deinit(services->posix_fs);
    }
    memset(services, 0, sizeof(*services));
}

#endif
