#ifndef PXA_ESP_SERVICES_H
#define PXA_ESP_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#include "pxa/audio.h"
#include "pxa/device.h"
#include "pxa/fs.h"
#include "pxa/game_render.h"
#include "pxa/surface.h"
#include "pxa/ipc.h"
#include "pxa/lease.h"
#include "pxa/net.h"
#include "pxa/package.h"
#include "pxa/permission.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/posix/pxa_posix_scheduler_store.h"
#include "pxa/posix/pxa_posix_storage.h"
#include "pxa/scheduler.h"
#include "pxa/sensor.h"
#include "pxa/storage.h"
#include "pxa/ui.h"
#include "pxa/window.h"

#include "pxa_esp_net.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PXA_ESP_SERVICES_MAX_COMPONENT_ID_BYTES 65

typedef struct {
    pxa_ui_service_t *ui;
    void *ui_workspace;

    pxa_permission_service_t *permission;
    void *permission_workspace;
    pxa_permission_declaration_t *declarations;
    uint16_t declaration_count;

    pxa_fs_service_t *fs;
    void *fs_workspace;
    pxa_posix_fs_t *posix_fs;
    void *posix_fs_workspace;
    pxa_fs_backend_t fs_backend;

    pxa_storage_service_t *storage;
    void *storage_workspace;
    pxa_posix_storage_t *posix_storage;
    void *posix_storage_workspace;
    pxa_storage_backend_t storage_backend;
    pxa_storage_backend_t loaded_storage_backend;
    pxa_posix_storage_config_t storage_config;
    void *storage_allocator_context;
    void *(*storage_allocate)(void *context, size_t size);

    pxa_ipc_broker_t *ipc;
    void *ipc_workspace;

    pxa_lease_service_t *lease;
    void *lease_workspace;

    pxa_sensor_service_t *sensor;
    void *sensor_workspace;

    pxa_device_service_t *device;
    void *device_workspace;

    pxa_net_service_t *net;
    void *net_workspace;

    pxa_audio_service_t *audio;
    void *audio_workspace;

    pxa_surface_service_t *surface;
    void *surface_workspace;

    pxa_game_render_service_t *game_render;
    void *game_render_workspace;

    pxa_scheduler_service_t *scheduler;
    void *scheduler_workspace;
    pxa_posix_scheduler_store_t *scheduler_store;
    void *scheduler_store_workspace;

    pxa_window_service_t *window;
    void *window_workspace;

    char (*job_components)[PXA_ESP_SERVICES_MAX_COMPONENT_ID_BYTES];
    pxa_bytes_t *job_component_ids;
    uint16_t job_component_count;
} pxa_esp_services_t;

typedef void *(*pxa_esp_services_allocate_fn)(void *context, size_t size);

typedef enum {
    PXA_ESP_SERVICES_ISSUE_NONE = 0,
    PXA_ESP_SERVICES_ISSUE_INVALID_PERMISSIONS,
    PXA_ESP_SERVICES_ISSUE_REQUIRED_PERMISSION_DENIED,
    PXA_ESP_SERVICES_ISSUE_INVALID_JOB,
} pxa_esp_services_issue_t;

typedef struct {
    pxa_runtime_t *runtime;
    const pxa_package_manifest_t *manifest;
    const char *identity;
    void *allocator_context;
    pxa_esp_services_allocate_fn allocate;
    void *permission_prompt_context;
    pxa_permission_prompt_fn permission_prompt;
    void *net_notify_context;
    pxa_esp_net_notify_fn net_notify;
    void *work_context;
    pxa_work_complete_fn complete_work;
    pxa_work_cancel_fn cancel_work;
    uint64_t work_epoch;
    pxa_ui_color_scheme_t color_scheme;
} pxa_esp_services_config_t;

typedef struct {
    const char *stage;
    pxa_esp_services_issue_t issue;
} pxa_esp_services_result_t;

pxa_status_t pxa_esp_services_initialize(
    pxa_esp_services_t *services, const pxa_esp_services_config_t *config,
    pxa_esp_services_result_t *result);
void pxa_esp_services_destroy(pxa_esp_services_t *services);

#ifdef __cplusplus
}
#endif

#endif
