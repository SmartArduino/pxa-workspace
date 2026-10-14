/* Pure-C PXA simulator host. Implements the simulator_pxa_* interface from
 * pxa_package_catalog.h entirely on the libpxa C stack: the C installer, C
 * services, C WAMR engine and C LVGL UI backend. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <curl/curl.h>
#include <SDL2/SDL.h>

#include "app_pages.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "pxa_package_catalog.h"
#include "font_awesome.h"

#include "pxa/activation.h"
#include "pxa/audio.h"
#include "pxa/clock.h"
#include "pxa/game_render.h"
#include "pxa/pxa_surface_transform.h"
#include "pxa/device.h"
#include "pxa/fs.h"
#include "pxa/surface.h"
#include "pxa/ipc.h"
#include "pxa/lease.h"
#include "pxa/lvgl/pxa_lvgl_ui.h"
#include "pxa/net.h"
#include "pxa/openssl/pxa_openssl.h"
#include "pxa/package.h"
#include "pxa/permission.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/posix/pxa_posix_clock.h"
#include "pxa/posix/pxa_posix_installer.h"
#include "pxa/posix/pxa_posix_scheduler_store.h"
#include "pxa/posix/pxa_posix_storage.h"
#include "pxa/runtime.h"
#include "pxa/scheduler.h"
#include "pxa/sensor.h"
#include "pxa/service.h"
#include "pxa/storage.h"
#include "pxa/ui.h"
#include "pxa/wamr/pxa_wamr_engine.h"
#include "pxa/wasi.h"
#include "pxa/window.h"

#define PXA_APP_PAGES_GAME_CONTEXT UINT8_C(0x80) /* Host-private, never part of Surface ABI. */
#define PXA_APP_PAGES_RASTER_CAPS PXA_RASTER_CAP_KNOWN_MASK
#define PXA_C_HOST_MAX_APPS 32
#define PXA_C_HOST_MAX_APP_ID 64
#define PXA_C_HOST_MAX_PATH 512
#define PXA_C_HOST_MAX_ICON_BYTES (64u * 1024u)
#define PXA_C_HOST_MAX_ICON_DIMENSION 256u
#define PXA_C_HOST_MAX_APP_PERMISSIONS APP_PAGES_APP_PERMISSION_MAX
#define PXA_C_HOST_MAX_DECLARATIONS \
    (PXA_C_HOST_MAX_APPS * PXA_C_HOST_MAX_APP_PERMISSIONS)
#define PXA_C_HOST_PERMISSION_NAME_MAX APP_PAGES_APP_PERMISSION_NAME_MAX
#define PXA_C_HOST_PERMISSION_SCOPE_MAX 1025
#define PXA_C_HOST_MAX_ACTIVE_COMPONENTS 16
#define PXA_C_HOST_MAX_JOBS 16
#define PXA_C_HOST_LAB_MODULE_COUNT 18u
#define PXA_C_HOST_ARCADE_GAME_COUNT 6u
#define PXA_C_HOST_NET_MAX_OPERATIONS 4
#define PXA_C_HOST_NET_RESPONSE_BYTES 4096u
#define PXA_C_HOST_NET_URL_BYTES (PXA_NET_MAX_URL_BYTES + 1u)
#define PXA_C_HOST_NET_HEADER_NAME_STORAGE (PXA_NET_MAX_HEADER_NAME_BYTES + 1u)
#define PXA_C_HOST_NET_HEADER_VALUE_STORAGE (PXA_NET_MAX_HEADER_VALUE_BYTES + 1u)
#define PXA_C_HOST_AUDIO_MAX_SESSIONS 4
#define PXA_C_HOST_AUDIO_SAMPLE_RATE 16000u
#define PXA_C_HOST_AUDIO_CHANNELS 1u
#define PXA_C_HOST_AUDIO_FRAME_MS 20u
#define PXA_C_HOST_AUDIO_MAX_QUEUE_MS 500u
#define PXA_C_HOST_AUDIO_MAX_QUEUE_BYTES \
    (PXA_C_HOST_AUDIO_SAMPLE_RATE * PXA_C_HOST_AUDIO_CHANNELS * 2u * \
     PXA_C_HOST_AUDIO_MAX_QUEUE_MS / 1000u)
#define PXA_C_HOST_EVENT_DRAIN_ROUNDS 3
#define PXA_C_HOST_BACK_EVENT_DRAIN_LIMIT 32
#define PXA_C_HOST_POINTER_DOWN UINT8_C(0)
#define PXA_C_HOST_WORK_CANCEL_GRACE_US UINT64_C(500000)

#define PXA_CONFIG_SERVICE_VERSION UINT16_C(4)
#define PXA_SYSTEM_CONFIG_ENVIRONMENT UINT16_C(12)
#define PXA_SYSTEM_CONFIGURATION_LOCALE UINT16_C(1)
#define PXA_SYSTEM_CONFIGURATION_TEXT_DIRECTION UINT16_C(0x8002)
#define PXA_SYSTEM_LOCALE_MAX_BYTES 63u

typedef struct {
    uint16_t declaration_index;
    uint8_t required;
    uint8_t granted;
} c_host_app_permission_t;

typedef struct {
    char id[PXA_C_HOST_MAX_APP_ID];
    char name[64];
    char version[64];
    char root[PXA_C_HOST_MAX_PATH];
    char source[PXA_C_HOST_MAX_PATH];
    char icon_path[PXA_C_HOST_MAX_PATH];
    uint8_t publisher_key_id[PXA_PACKAGE_DIGEST_BYTES];
    uint8_t enabled;
    uint8_t installed;
    uint8_t staged;
    uint8_t built_in;
    uint8_t has_publisher_key_id;
    pxa_bytes_t job_components[PXA_C_HOST_MAX_JOBS];
    char job_component_names[PXA_C_HOST_MAX_JOBS][65];
    uint16_t job_component_count;
    c_host_app_permission_t permissions[PXA_C_HOST_MAX_APP_PERMISSIONS];
    uint16_t permission_count;
} pxa_c_host_app_t;

typedef struct pxa_c_host_ui_asset {
    struct pxa_c_host_ui_asset *next;
    lv_image_dsc_t descriptor;
    uint8_t *bytes;
    size_t size;
    size_t references;
    uint64_t last_used;
    char path[257];
} pxa_c_host_ui_asset_t;

typedef struct {
    CURL *easy;
    struct curl_slist *request_headers;
    uint64_t operation;
    size_t body_offset;
    size_t body_size;
    size_t body_capacity;
    size_t content_type_size;
    uint32_t timeout_ms;
    uint16_t method;
    uint16_t wanted_header_count;
    uint8_t abi_minor;
    uint8_t active;
    uint8_t complete;
    uint8_t body_limit_exceeded;
    pxa_status_t result;
    uint16_t status_code;
    uint16_t response_header_count;
    char url[PXA_C_HOST_NET_URL_BYTES];
    char content_type[PXA_NET_MAX_CONTENT_TYPE_BYTES + 1u];
    char wanted_header_names[PXA_NET_MAX_HEADERS]
                            [PXA_C_HOST_NET_HEADER_NAME_STORAGE];
    char response_header_values[PXA_NET_MAX_HEADERS]
                               [PXA_C_HOST_NET_HEADER_VALUE_STORAGE];
    pxa_net_header_t response_headers[PXA_NET_MAX_HEADERS];
    uint8_t body[PXA_C_HOST_NET_RESPONSE_BYTES];
} c_host_net_operation_t;

typedef struct {
    uint64_t id;
    uint64_t submitted_samples;
    uint64_t accepted_samples;
    uint8_t active;
    uint8_t committed;
    uint8_t asset_active;
    uint8_t asset_paused;
    uint8_t asset_loop;
    int16_t asset_gain_db_q8;
    char asset_path[PXA_C_HOST_MAX_PATH];
} c_host_audio_session_t;

typedef struct {
    lv_obj_t *object;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} c_host_surface_fragment_t;

#define C_HOST_SURFACE_MAX_FRAGMENTS \
    ((2u * PXA_SURFACE_MAX_OPAQUE_UI_REGIONS + 1u) * \
     (PXA_SURFACE_MAX_OPAQUE_UI_REGIONS + 1u))

typedef struct {
    uint8_t *buffers[3];
    uint8_t *mapped_buffers[3];
    uint8_t *display_buffer;
    lv_image_dsc_t image;
    lv_image_dsc_t ui_alpha_image;
    lv_obj_t *ui_alpha_object;
    uint8_t *ui_alpha_bytes;
    size_t ui_alpha_capacity;
    c_host_surface_fragment_t fragments[C_HOST_SURFACE_MAX_FRAGMENTS];
    pxa_surface_damage_rect_t opaque_ui_regions
        [PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    pxa_surface_layer_t layer;
    uint32_t frame_bytes;
    uint32_t stride_bytes;
    uint32_t display_frame_bytes;
    uint32_t display_stride_bytes;
    uint16_t width;
    uint16_t height;
    uint16_t display_width;
    uint16_t display_height;
    uint16_t format;
    uint8_t flags;
    uint8_t buffer_count;
    uint8_t fragment_count;
    uint8_t opaque_ui_region_count;
    uint8_t mapped_registered;
    uint8_t release_head;
    uint8_t release_count;
    uint8_t release_pending_mask;
    uint8_t scale;
    uint8_t has_display_frame;
    int8_t staged;
    int8_t current;
    int8_t writing;
    int8_t pending;
    uint64_t pending_frame_id;
    uint64_t last_frame_id;
    uint64_t submitted_frames;
    uint64_t presented_frames;
    uint64_t dropped_frames;
    uint64_t replaced_frames;
    uint64_t released_frames;
    pxa_surface_release_t releases[3];
    uint8_t *raster_textures[PXA_RASTER_MAX_TEXTURES];
    uint16_t raster_texture_width[PXA_RASTER_MAX_TEXTURES];
    uint16_t raster_texture_height[PXA_RASTER_MAX_TEXTURES];
    uint16_t *raster_palette;
    uint16_t raster_palette_levels;
    uint16_t *raster_depth;
    uint8_t raster_scratch_mode;
    uint32_t raster_draw_capacity;
    uint8_t *raster_draw_lists[2];
    uint32_t raster_draw_sizes[2];
    uint64_t raster_draw_queued_us[2];
    int8_t raster_draw_pending;
    uint64_t raster_last_frame_id;
    pxa_raster_telemetry_t raster_telemetry;
} c_host_surface_t;

typedef struct {
    void *workspace;
    size_t workspace_size;
    uint8_t *encoded;
    size_t encoded_capacity;
    pxa_package_limits_t parsed_limits;
    uint8_t has_parsed_limits;
} c_host_manifest_buffers_t;

typedef struct {
    uint8_t initialized;
    uint8_t ui_stress_profile;
    char storage_root[PXA_C_HOST_MAX_PATH];
    char builtins_root[PXA_C_HOST_MAX_PATH];
    char inbox_root[PXA_C_HOST_MAX_PATH];
    pxa_runtime_t *runtime;
    void *runtime_workspace;
    pxa_posix_storage_t *posix_storage;
    pxa_storage_backend_t storage_backend;
    pxa_storage_config_t storage_config;
    pxa_storage_service_t *storage_service;
    void *storage_workspace;
    void *storage_service_workspace;
    pxa_posix_fs_t *posix_fs;
    pxa_fs_backend_t fs_backend;
    pxa_fs_config_t fs_config;
    pxa_fs_service_t *fs_service;
    void *fs_service_workspace;
    void *fs_service_state_workspace;
    pxa_window_service_t *window_service;
    void *window_workspace;
    pxa_ipc_broker_t *ipc_broker;
    void *ipc_broker_workspace;
    pxa_lease_service_t *lease_service;
    void *lease_workspace;
    pxa_permission_service_t *permission_service;
    void *permission_workspace;
    size_t permission_workspace_size;
    pxa_scheduler_service_t *scheduler_service;
    void *scheduler_workspace;
    pxa_posix_scheduler_store_t *posix_scheduler_store;
    void *scheduler_store_workspace;
    pxa_scheduler_store_t scheduler_store;
    pxa_sensor_service_t *sensor_service;
    void *sensor_workspace;
    pxa_device_service_t *device_service;
    void *device_workspace;
    pxa_net_service_t *net_service;
    void *net_workspace;
    pxa_audio_service_t *audio_service;
    void *audio_workspace;
    pxa_surface_service_t *surface_service;
    void *surface_workspace;
    pxa_game_render_service_t *game_render_service;
    void *game_render_workspace;
    c_host_surface_t *surface;
    pxa_ui_service_t *ui_service;
    void *ui_workspace;
    pxa_lvgl_ui_t *ui_adapter;
    void *ui_adapter_workspace;
    pxa_ui_backend_t ui_backend;
    pxa_component_t ui_component;
    pxa_wamr_engine_t *engine;
    pxa_component_engine_t engine_ops;
    void *engine_workspace;
    pxa_posix_installer_t *installer;
    void *installer_workspace;
    c_host_manifest_buffers_t result_manifest_buffers;
    pxa_activation_coordinator_t *coordinator;
    void *coordinator_workspace;
    void *plan_workspace;
    pxa_component_t active_component;
    pxa_component_t active_components[PXA_C_HOST_MAX_ACTIVE_COMPONENTS];
    uint8_t active_component_count;
    uint32_t pointer_probe_count;
    uint32_t ui_event_probe_count;
    uint64_t next_instance_id;
    pxa_component_t active_jobs[PXA_C_HOST_MAX_JOBS];
    uint64_t job_stop_at_us[PXA_C_HOST_MAX_JOBS];
    uint64_t job_force_stop_at_us[PXA_C_HOST_MAX_JOBS];
    pxa_scheduler_entry_t active_work[PXA_C_HOST_MAX_JOBS];
    pxa_work_result_t work_result[PXA_C_HOST_MAX_JOBS];
    int16_t activating_job_slot;
    pxa_c_host_app_t *active_app;
    pxa_package_manifest_t *active_manifest;
    pxa_c_host_ui_asset_t *ui_assets;
    size_t ui_asset_bytes;
    uint64_t ui_asset_clock;
    pxa_package_limits_t package_limits;
    pxa_c_host_app_t apps[PXA_C_HOST_MAX_APPS];
    size_t app_count;
    char disabled_apps[PXA_C_HOST_MAX_APPS][PXA_C_HOST_MAX_APP_ID];
    size_t disabled_app_count;
    pxa_permission_declaration_t declarations[PXA_C_HOST_MAX_DECLARATIONS];
    char declaration_name[PXA_C_HOST_MAX_DECLARATIONS]
                         [PXA_C_HOST_PERMISSION_NAME_MAX];
    char declaration_scope[PXA_C_HOST_MAX_DECLARATIONS]
                          [PXA_C_HOST_PERMISSION_SCOPE_MAX];
    uint16_t declaration_count;
    uint16_t clock_period_ms;
    uint64_t clock_last_tick_us;
    uint64_t fake_now_us;
    char locale[PXA_SYSTEM_LOCALE_MAX_BYTES + 1u];
    uint8_t locale_size;
    uint8_t text_direction;
    uint32_t maintenance_last_tick_ms;
    lv_timer_t *maintenance_timer;
    uint8_t ui_commit_ready;
    uint8_t publisher_key[1024];
    size_t publisher_key_size;
    pxa_openssl_publisher_key_t publisher_key_entry;
    pxa_openssl_trust_t publisher_trust;
    struct {
        uint8_t active;
        uint32_t prompt_id;
        pxa_component_t component;
        pxa_request_token_t request_token;
        uint16_t permission_index;
    } permission_prompt;
    uint32_t next_permission_prompt_id;
    c_host_net_operation_t net_operations[PXA_C_HOST_NET_MAX_OPERATIONS];
    uint64_t next_net_operation;
    CURLM *curl_multi;
    uint8_t curl_initialized;
    c_host_audio_session_t
        audio_sessions[PXA_C_HOST_AUDIO_MAX_SESSIONS];
    uint64_t next_audio_session;
    SDL_AudioDeviceID audio_device;
    uint8_t audio_subsystem_owned;
} pxa_c_host_t;

static struct {
    uint32_t width;
    uint32_t height;
    uint32_t safe_insets[4]; /* top, right, bottom, left */
} g_display_metrics = {320, 240, {0, 0, 0, 0}};

static pxa_c_host_t g_host;
static int c_host_surface_process_pending(void);

void simulator_pxa_set_display_metrics(uint32_t width, uint32_t height,
                                       const uint32_t safe_insets[4]) {
    uint32_t index;
    pxa_ui_environment_t environment;
    pxa_window_snapshot_t snapshot;
    if (width == 0 || height == 0) return;
    g_display_metrics.width = width;
    g_display_metrics.height = height;
    for (index = 0; index < 4; ++index)
        g_display_metrics.safe_insets[index] =
            safe_insets != NULL ? safe_insets[index] : 0u;
    if (!g_host.initialized || g_host.ui_component == PXA_COMPONENT_INVALID)
        return;
    if (g_host.ui_service != NULL &&
        pxa_ui_get_environment(g_host.ui_service, g_host.ui_component,
                               PXA_UI_PRIMARY_SURFACE, &environment) ==
            PXA_STATUS_OK) {
        environment.width = width;
        environment.height = height;
        memcpy(environment.safe_insets, g_display_metrics.safe_insets,
               sizeof(environment.safe_insets));
        (void)pxa_ui_update_environment(g_host.ui_service, g_host.ui_component,
                                        &environment);
    }
    if (g_host.window_service != NULL) {
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.logical_width = width;
        snapshot.logical_height = height;
        snapshot.pixel_width = width;
        snapshot.pixel_height = height;
        snapshot.safe_insets.top = g_display_metrics.safe_insets[0];
        snapshot.safe_insets.right = g_display_metrics.safe_insets[1];
        snapshot.safe_insets.bottom = g_display_metrics.safe_insets[2];
        snapshot.safe_insets.left = g_display_metrics.safe_insets[3];
        snapshot.density_numerator = 1;
        snapshot.density_denominator = 1;
        snapshot.focused = 1;
        (void)pxa_window_update_snapshot(g_host.window_service,
                                         g_host.ui_component, &snapshot);
    }
}

bool simulator_pxa_set_locale(const char *locale, uint8_t text_direction) {
    size_t size;
    if (locale == NULL || text_direction > 1u) return false;
    size = strlen(locale);
    if (size < 2u || size > PXA_SYSTEM_LOCALE_MAX_BYTES) return false;
    memcpy(g_host.locale, locale, size + 1u);
    g_host.locale_size = (uint8_t)size;
    g_host.text_direction = text_direction;
    return true;
}

static simulator_pxa_launch_request_fn g_system_launch_request;
static simulator_pxa_close_request_fn g_system_close_request;
static simulator_pxa_catalog_changed_fn g_system_catalog_changed;
static simulator_pxa_window_changed_fn g_system_window_changed;
static void *g_system_callback_context;

static void *c_host_ui_alloc(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void c_host_ui_free(void *context, void *memory) {
    (void)context;
    free(memory);
}

static void *c_host_artifact_alloc(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void c_host_artifact_free(void *context, void *memory) {
    (void)context;
    free(memory);
}

static uint32_t c_host_ui_max_png_bytes(void) {
    return g_host.ui_stress_profile ? 1024u * 1024u : 256u * 1024u;
}

static uint32_t c_host_ui_max_png_dimension(void) {
    return g_host.ui_stress_profile ? 1024u : 512u;
}

static size_t c_host_ui_asset_cache_bytes(void) {
    return g_host.ui_stress_profile ? 8u * 1024u * 1024u
                                    : 1024u * 1024u;
}

static void c_host_manifest_buffers_release(
    c_host_manifest_buffers_t *buffers) {
    if (buffers == NULL) return;
    free(buffers->encoded);
    free(buffers->workspace);
    memset(buffers, 0, sizeof(*buffers));
}

static int c_host_manifest_buffers_ensure_encoded(
    c_host_manifest_buffers_t *buffers, size_t required) {
    uint8_t *resized;
    if (buffers == NULL || required == 0) return 0;
    if (buffers->encoded_capacity >= required) return 1;
    resized = realloc(buffers->encoded, required);
    if (resized == NULL) return 0;
    buffers->encoded = resized;
    buffers->encoded_capacity = required;
    return 1;
}

static int c_host_manifest_buffers_ensure_workspace(
    c_host_manifest_buffers_t *buffers, size_t required) {
    void *resized;
    if (buffers == NULL || required == 0) return 0;
    if (buffers->workspace_size >= required) return 1;
    resized = realloc(buffers->workspace, required);
    if (resized == NULL) return 0;
    buffers->workspace = resized;
    buffers->workspace_size = required;
    return 1;
}

static int c_host_manifest_buffers_prepare_workspace(
    c_host_manifest_buffers_t *buffers, size_t encoded_size) {
    pxa_package_limits_t actual_limits;
    size_t required;
    if (buffers == NULL || buffers->encoded == NULL || encoded_size == 0 ||
        encoded_size > buffers->encoded_capacity ||
        pxa_package_manifest_measure(
            (pxa_bytes_t){buffers->encoded, encoded_size},
            &g_host.package_limits, &actual_limits) != PXA_STATUS_OK) {
        return 0;
    }
    required = pxa_package_manifest_workspace_size(&actual_limits);
    if (required == 0 ||
        !c_host_manifest_buffers_ensure_workspace(buffers, required)) {
        return 0;
    }
    buffers->parsed_limits = actual_limits;
    buffers->has_parsed_limits = 1;
    return 1;
}

static pxa_c_host_app_t *c_host_find_app(const char *id);
static void c_host_stop_active(void);
static int c_host_read_manifest(const char *root,
                                c_host_manifest_buffers_t *buffers,
                                pxa_package_manifest_t **manifest);
static int c_host_verify_package_source(
    const char *source, c_host_manifest_buffers_t *buffers,
    pxa_package_manifest_t **manifest);
static void c_host_rebase_app_views(pxa_c_host_app_t *app);
static void c_host_copy_manifest_metadata(
    pxa_c_host_app_t *app, const pxa_package_manifest_t *manifest);

static void c_host_clear_active_components(void) {
    g_host.active_component_count = 0;
}

static void c_host_clear_active_jobs(void) {
    memset(g_host.active_jobs, 0, sizeof(g_host.active_jobs));
    memset(g_host.job_stop_at_us, 0, sizeof(g_host.job_stop_at_us));
    memset(g_host.job_force_stop_at_us, 0,
           sizeof(g_host.job_force_stop_at_us));
    memset(g_host.active_work, 0, sizeof(g_host.active_work));
    memset(g_host.work_result, 0, sizeof(g_host.work_result));
    g_host.activating_job_slot = -1;
}

static int c_host_track_active_component(pxa_component_t component) {
    uint8_t index;
    if (component == PXA_COMPONENT_INVALID) return 0;
    for (index = 0; index < g_host.active_component_count; ++index) {
        if (g_host.active_components[index] == component) return 1;
    }
    if (g_host.active_component_count >= PXA_C_HOST_MAX_ACTIVE_COMPONENTS) {
        return 0;
    }
    g_host.active_components[g_host.active_component_count++] = component;
    return 1;
}

static void c_host_untrack_active_component(pxa_component_t component) {
    uint8_t index;
    for (index = 0; index < g_host.active_component_count; ++index) {
        if (g_host.active_components[index] == component) {
            memmove(&g_host.active_components[index],
                    &g_host.active_components[index + 1u],
                    (size_t)(g_host.active_component_count - index - 1u) *
                        sizeof(g_host.active_components[0]));
            --g_host.active_component_count;
            return;
        }
    }
}

static int c_host_deliver_active_event_round(void) {
    uint8_t index;
    int delivered = 0;
    for (index = 0; index < g_host.active_component_count; ++index) {
        const pxa_status_t status = pxa_wamr_engine_deliver_event(
            g_host.engine, g_host.runtime, g_host.active_components[index]);
        if (status == PXA_STATUS_OK) delivered = 1;
    }
    return delivered;
}

static void c_host_drain_active_events(void) {
    uint8_t round;
    for (round = 0; round < PXA_C_HOST_EVENT_DRAIN_ROUNDS; ++round) {
        const int delivered = c_host_deliver_active_event_round();
        const int presented = c_host_surface_process_pending();
        const int32_t released = g_host.surface_service == NULL ? 0 :
            pxa_surface_service_flush_releases(g_host.surface_service);
        if (!delivered && !presented && released <= 0) return;
    }
}

LV_FONT_DECLARE(font_awesome_20_4)

typedef struct {
    lv_image_dsc_t descriptor;
    uint8_t *bytes;
} pxa_c_host_icon_t;

/* --- platform helpers -------------------------------------------------- */

static uint64_t host_now_us(void *context) {
    (void)context;
    return g_host.fake_now_us;
}

static uint64_t c_host_scheduler_clock_ms(void *context) {
    (void)context;
    return g_host.fake_now_us / 1000u;
}

static int c_host_job_component_slot(pxa_bytes_t component_id) {
    uint16_t index;
    if (g_host.active_app == NULL) return -1;
    for (index = 0; index < g_host.active_app->job_component_count; ++index) {
        if (g_host.active_app->job_components[index].size == component_id.size &&
            memcmp(g_host.active_app->job_components[index].data,
                   component_id.data,
                   component_id.size) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int c_host_has_active_jobs(void) {
    uint16_t index;
    if (g_host.active_app == NULL) return 0;
    for (index = 0; index < g_host.active_app->job_component_count; ++index) {
        if (g_host.active_jobs[index] != PXA_COMPONENT_INVALID) return 1;
    }
    return 0;
}

static pxa_status_t c_host_complete_work(void *context,
                                         pxa_component_t component,
                                         uint32_t work_id,
                                         pxa_work_result_t result) {
    uint16_t index;
    (void)context;
    if (g_host.active_app == NULL) return PXA_STATUS_BAD_STATE;
    for (index = 0; index < g_host.active_app->job_component_count; ++index) {
        if (g_host.active_jobs[index] == component &&
            g_host.active_work[index].id == work_id &&
            g_host.work_result[index] == 0) {
            g_host.work_result[index] = result;
            return PXA_STATUS_OK;
        }
    }
    if (g_host.activating_job_slot >= 0 &&
        (uint16_t)g_host.activating_job_slot <
            g_host.active_app->job_component_count &&
        g_host.active_work[g_host.activating_job_slot].id == work_id &&
        g_host.work_result[g_host.activating_job_slot] == 0) {
        g_host.work_result[g_host.activating_job_slot] = result;
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_NOT_FOUND;
}

static void c_host_defer_work(const pxa_scheduler_entry_t *entry) {
    (void)pxa_scheduler_defer(g_host.scheduler_service, entry,
                              PXA_SCHEDULER_DEFAULT_MIN_DELAY_MS);
}

static pxa_status_t c_host_cancel_work(void *context, uint32_t work_id) {
    uint16_t index;
    (void)context;
    if (g_host.active_app == NULL) return PXA_STATUS_BAD_STATE;
    for (index = 0; index < g_host.active_app->job_component_count; ++index) {
        if (g_host.active_jobs[index] != PXA_COMPONENT_INVALID &&
            g_host.active_work[index].id == work_id &&
            g_host.work_result[index] == 0) {
            g_host.work_result[index] = PXA_WORK_RESULT_FAILURE;
            return PXA_STATUS_OK;
        }
    }
    return PXA_STATUS_NOT_FOUND;
}

static void c_host_finish_job(uint16_t index, pxa_work_result_t result) {
    const pxa_component_t component = g_host.active_jobs[index];
    const pxa_scheduler_entry_t entry = g_host.active_work[index];
    if (component == PXA_COMPONENT_INVALID) return;
    if (result == PXA_WORK_RESULT_RETRY &&
        entry.attempt < entry.max_attempts) {
        (void)pxa_scheduler_retry(g_host.scheduler_service, &entry);
    }
    (void)pxa_activation_deactivate(g_host.coordinator,
                                     g_host.active_app->job_components[index],
                                     PXA_STOP_NORMAL);
    c_host_untrack_active_component(component);
    g_host.active_jobs[index] = PXA_COMPONENT_INVALID;
    g_host.job_stop_at_us[index] = 0;
    g_host.job_force_stop_at_us[index] = 0;
    memset(&g_host.active_work[index], 0, sizeof(g_host.active_work[index]));
    g_host.work_result[index] = 0;
}

static void c_host_maintain_jobs(void) {
    uint16_t index;
    if (g_host.active_app == NULL) return;
    for (index = 0; index < g_host.active_app->job_component_count; ++index) {
        const pxa_component_t component = g_host.active_jobs[index];
        const pxa_scheduler_entry_t *entry = &g_host.active_work[index];
        if (component == PXA_COMPONENT_INVALID) continue;
        if (g_host.work_result[index] != 0) {
            c_host_finish_job(index, g_host.work_result[index]);
        } else if (g_host.fake_now_us >= g_host.job_stop_at_us[index]) {
            if (g_host.job_force_stop_at_us[index] == 0) {
                if (pxa_scheduler_post_work_stop(
                        g_host.scheduler_service, component, entry->id,
                        g_host.job_stop_at_us[index] / 1000u) ==
                    PXA_STATUS_OK) {
                    g_host.job_force_stop_at_us[index] =
                        g_host.fake_now_us + PXA_C_HOST_WORK_CANCEL_GRACE_US;
                } else {
                    c_host_finish_job(index, PXA_WORK_RESULT_RETRY);
                }
            } else if (g_host.fake_now_us >=
                       g_host.job_force_stop_at_us[index]) {
                c_host_finish_job(index, PXA_WORK_RESULT_RETRY);
            }
        }
    }
    if (g_host.active_component == PXA_COMPONENT_INVALID &&
        !c_host_has_active_jobs() &&
        !pxa_scheduler_has_pending(g_host.scheduler_service)) {
        c_host_stop_active();
    }
}

static uint64_t host_now_us_real(void *context) {
    (void)context;
    return (uint64_t)lv_tick_get() * 1000u;
}

static uint32_t read_be_u32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

static void release_icon(void *user_data) {
    pxa_c_host_icon_t *icon = (pxa_c_host_icon_t *)user_data;
    if (icon == NULL) return;
    free(icon->bytes);
    free(icon);
}

static app_pages_app_icon_t load_icon(const pxa_c_host_app_t *app) {
    static const uint8_t png_signature[] = {0x89, 0x50, 0x4e, 0x47,
                                            0x0d, 0x0a, 0x1a, 0x0a};
    app_pages_app_icon_t result;
    pxa_c_host_icon_t *icon;
    char path[PXA_C_HOST_MAX_PATH];
    struct stat metadata;
    int fd;
    size_t offset = 0;

    memset(&result, 0, sizeof(result));
    if (app == NULL || app->icon_path[0] == '\0') {
        return result;
    }
    if (snprintf(path, sizeof(path), "%s/%s", app->root, app->icon_path) >=
        (int)sizeof(path))
        return result;
    fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &metadata) != 0 || metadata.st_size < 24 ||
        (uintmax_t)metadata.st_size > PXA_C_HOST_MAX_ICON_BYTES) {
        if (fd >= 0) close(fd);
        return result;
    }
    icon = (pxa_c_host_icon_t *)calloc(1, sizeof(*icon));
    if (icon == NULL) {
        close(fd);
        return result;
    }
    icon->bytes = (uint8_t *)malloc((size_t)metadata.st_size);
    if (icon->bytes == NULL) {
        close(fd);
        free(icon);
        return result;
    }
    while (offset < (size_t)metadata.st_size) {
        ssize_t count = read(fd, icon->bytes + offset,
                             (size_t)metadata.st_size - offset);
        if (count <= 0) {
            close(fd);
            release_icon(icon);
            return result;
        }
        offset += (size_t)count;
    }
    close(fd);
    if (memcmp(icon->bytes, png_signature, sizeof(png_signature)) != 0 ||
        memcmp(icon->bytes + 12, "IHDR", 4) != 0 ||
        read_be_u32(icon->bytes + 16) == 0 ||
        read_be_u32(icon->bytes + 20) == 0 ||
        read_be_u32(icon->bytes + 16) > PXA_C_HOST_MAX_ICON_DIMENSION ||
        read_be_u32(icon->bytes + 20) > PXA_C_HOST_MAX_ICON_DIMENSION) {
        release_icon(icon);
        return result;
    }
    icon->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon->descriptor.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    icon->descriptor.header.w = read_be_u32(icon->bytes + 16);
    icon->descriptor.header.h = read_be_u32(icon->bytes + 20);
    icon->descriptor.data_size = (size_t)metadata.st_size;
    icon->descriptor.data = icon->bytes;
    result.image_dsc = &icon->descriptor;
    result.release_cb = release_icon;
    result.release_user_data = icon;
    return result;
}

static void c_host_release_ui_asset(pxa_c_host_ui_asset_t *asset) {
    if (asset == NULL) return;
    if (asset->descriptor.header.magic == LV_IMAGE_HEADER_MAGIC) {
        lv_image_cache_drop(&asset->descriptor);
    }
    free(asset->bytes);
    free(asset);
}

static void c_host_clear_ui_assets(void) {
    while (g_host.ui_assets != NULL) {
        pxa_c_host_ui_asset_t *asset = g_host.ui_assets;
        g_host.ui_assets = asset->next;
        c_host_release_ui_asset(asset);
    }
    g_host.ui_asset_bytes = 0;
    g_host.ui_asset_clock = 0;
}

static int c_host_evict_ui_assets(size_t required) {
    size_t budget = c_host_ui_asset_cache_bytes();
    while (g_host.ui_asset_bytes > budget ||
           required > budget - g_host.ui_asset_bytes) {
        pxa_c_host_ui_asset_t *asset;
        pxa_c_host_ui_asset_t *previous = NULL;
        pxa_c_host_ui_asset_t *victim = NULL;
        pxa_c_host_ui_asset_t *victim_previous = NULL;
        for (asset = g_host.ui_assets; asset != NULL; asset = asset->next) {
            if (asset->references == 0 &&
                (victim == NULL || asset->last_used < victim->last_used)) {
                victim = asset;
                victim_previous = previous;
            }
            previous = asset;
        }
        if (victim == NULL) return 0;
        if (victim_previous == NULL)
            g_host.ui_assets = victim->next;
        else
            victim_previous->next = victim->next;
        g_host.ui_asset_bytes -= victim->size;
        c_host_release_ui_asset(victim);
    }
    return 1;
}

static const void *c_host_resolve_ui_asset(const uint8_t *path,
                                           size_t path_size,
                                           void *user_data) {
    static const uint8_t png_signature[] = {0x89, 0x50, 0x4e, 0x47,
                                            0x0d, 0x0a, 0x1a, 0x0a};
    const pxa_package_file_t *file;
    pxa_c_host_ui_asset_t *asset;
    char full_path[PXA_C_HOST_MAX_PATH];
    struct stat metadata;
    size_t offset = 0;
    int fd;
    (void)user_data;
    if (path == NULL || path_size == 0 ||
        path_size > (g_host.ui_stress_profile ? 256u : 128u) ||
        g_host.active_manifest == NULL || g_host.active_app == NULL)
        return NULL;
    file = pxa_package_file_find(
        g_host.active_manifest, (pxa_bytes_t){path, path_size});
    if (file == NULL || file->size < 24 ||
        file->size > c_host_ui_max_png_bytes()) return NULL;
    for (asset = g_host.ui_assets; asset != NULL; asset = asset->next) {
        if (asset->size == file->size && strlen(asset->path) == path_size &&
            memcmp(asset->path, path, path_size) == 0) {
            ++asset->references;
            asset->last_used = ++g_host.ui_asset_clock;
            return &asset->descriptor;
        }
    }
    if (file->size > c_host_ui_asset_cache_bytes() ||
        !c_host_evict_ui_assets((size_t)file->size))
        return NULL;
    asset = (pxa_c_host_ui_asset_t *)calloc(1, sizeof(*asset));
    if (asset == NULL) return NULL;
    asset->bytes = (uint8_t *)malloc((size_t)file->size);
    if (asset->bytes == NULL) {
        free(asset);
        return NULL;
    }
    if (snprintf(full_path, sizeof(full_path), "%s/%.*s",
                 g_host.active_app->root, (int)path_size,
                 (const char *)path) >= (int)sizeof(full_path)) {
        c_host_release_ui_asset(asset);
        return NULL;
    }
    fd = open(full_path, O_RDONLY);
    if (fd < 0 || fstat(fd, &metadata) != 0 ||
        (uint64_t)metadata.st_size != file->size) {
        if (fd >= 0) close(fd);
        c_host_release_ui_asset(asset);
        return NULL;
    }
    while (offset < (size_t)file->size) {
        ssize_t count = read(fd, asset->bytes + offset,
                             (size_t)file->size - offset);
        if (count <= 0) {
            close(fd);
            c_host_release_ui_asset(asset);
            return NULL;
        }
        offset += (size_t)count;
    }
    close(fd);
    if (memcmp(asset->bytes, png_signature, sizeof(png_signature)) != 0 ||
        memcmp(asset->bytes + 12, "IHDR", 4) != 0 ||
        read_be_u32(asset->bytes + 16) == 0 ||
        read_be_u32(asset->bytes + 20) == 0 ||
        read_be_u32(asset->bytes + 16) > c_host_ui_max_png_dimension() ||
        read_be_u32(asset->bytes + 20) > c_host_ui_max_png_dimension()) {
        c_host_release_ui_asset(asset);
        return NULL;
    }
    memcpy(asset->path, path, path_size);
    asset->path[path_size] = '\0';
    asset->size = (size_t)file->size;
    asset->references = 1;
    asset->last_used = ++g_host.ui_asset_clock;
    asset->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    asset->descriptor.header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
    asset->descriptor.header.w = read_be_u32(asset->bytes + 16);
    asset->descriptor.header.h = read_be_u32(asset->bytes + 20);
    asset->descriptor.data_size = asset->size;
    asset->descriptor.data = asset->bytes;
    asset->next = g_host.ui_assets;
    g_host.ui_assets = asset;
    g_host.ui_asset_bytes += asset->size;
    return &asset->descriptor;
}

static void c_host_release_ui_asset_reference(const void *source,
                                              void *user_data) {
    pxa_c_host_ui_asset_t *asset;
    (void)user_data;
    for (asset = g_host.ui_assets; asset != NULL; asset = asset->next) {
        if (&asset->descriptor != source) continue;
        if (asset->references != 0) --asset->references;
        asset->last_used = ++g_host.ui_asset_clock;
        return;
    }
}

static int make_dirs(const char *path) {
    char buffer[PXA_C_HOST_MAX_PATH];
    size_t length = strlen(path);
    size_t index;
    if (length == 0 || length >= sizeof(buffer)) return 0;
    memcpy(buffer, path, length + 1);
    for (index = 1; index < length; ++index) {
        if (buffer[index] != '/') continue;
        buffer[index] = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) return 0;
        buffer[index] = '/';
    }
    return mkdir(buffer, 0700) == 0 || errno == EEXIST;
}

static int c_host_app_data_path(char *output, size_t capacity,
                                const pxa_c_host_app_t *app) {
    int size;
    if (output == NULL || capacity == 0 || app == NULL) return 0;
    size = snprintf(output, capacity, "%s/data/%s", g_host.storage_root,
                    app->id);
    return size > 0 && (size_t)size < capacity;
}

static int c_host_policy_app_id_valid(const char *app_id) {
    size_t index;
    size_t size;
    if (app_id == NULL) return 0;
    size = strlen(app_id);
    if (size == 0 || size >= PXA_C_HOST_MAX_APP_ID || app_id[0] < 'a' ||
        app_id[0] > 'z') {
        return 0;
    }
    for (index = 0; index < size; ++index) {
        const char value = app_id[index];
        if (!((value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '.' ||
              value == '_' || value == '-')) {
            return 0;
        }
    }
    return 1;
}

static size_t c_host_disabled_app_index(const char *app_id) {
    size_t index;
    for (index = 0; index < g_host.disabled_app_count; ++index) {
        if (strcmp(g_host.disabled_apps[index], app_id) == 0) return index;
    }
    return SIZE_MAX;
}

static void c_host_apply_disabled_policy(void) {
    size_t index;
    for (index = 0; index < g_host.app_count; ++index) {
        g_host.apps[index].enabled =
            c_host_disabled_app_index(g_host.apps[index].id) == SIZE_MAX;
    }
}

static void c_host_load_disabled_policy(void) {
    char path[PXA_C_HOST_MAX_PATH];
    char line[PXA_C_HOST_MAX_APP_ID + 8];
    FILE *input;
    int path_size = snprintf(path, sizeof(path), "%s/policy/disabled",
                             g_host.storage_root);
    g_host.disabled_app_count = 0;
    if (path_size <= 0 || (size_t)path_size >= sizeof(path)) {
        c_host_apply_disabled_policy();
        return;
    }
    input = fopen(path, "r");
    if (input == NULL) {
        c_host_apply_disabled_policy();
        return;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (c_host_policy_app_id_valid(line) &&
            c_host_disabled_app_index(line) == SIZE_MAX &&
            g_host.disabled_app_count < PXA_C_HOST_MAX_APPS) {
            snprintf(g_host.disabled_apps[g_host.disabled_app_count],
                     sizeof(g_host.disabled_apps[0]), "%s", line);
            ++g_host.disabled_app_count;
        }
    }
    fclose(input);
    c_host_apply_disabled_policy();
}

static int c_host_save_disabled_policy(void) {
    char directory[PXA_C_HOST_MAX_PATH];
    char path[PXA_C_HOST_MAX_PATH];
    char temporary[PXA_C_HOST_MAX_PATH];
    FILE *output;
    size_t index;
    int descriptor;
    int size = snprintf(directory, sizeof(directory), "%s/policy",
                        g_host.storage_root);
    if (size <= 0 || (size_t)size >= sizeof(directory) ||
        !make_dirs(directory)) {
        return 0;
    }
    size = snprintf(path, sizeof(path), "%s/disabled", directory);
    if (size <= 0 || (size_t)size >= sizeof(path)) return 0;
    size = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (size <= 0 || (size_t)size >= sizeof(temporary)) {
        return 0;
    }
    output = fopen(temporary, "w");
    if (output == NULL) return 0;
    for (index = 0; index < g_host.disabled_app_count; ++index) {
        if (fprintf(output, "%s\n", g_host.disabled_apps[index]) < 0) {
            fclose(output);
            unlink(temporary);
            return 0;
        }
    }
    descriptor = fileno(output);
    if (fflush(output) != 0 || descriptor < 0 || fsync(descriptor) != 0 ||
        fclose(output) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return 0;
    }
    return 1;
}

static int c_host_bind_app_data(const pxa_c_host_app_t* app) {
    pxa_posix_fs_config_t fs_config;
    pxa_posix_storage_config_t storage_config;
    char data_root[PXA_C_HOST_MAX_PATH];
    char storage_root[PXA_C_HOST_MAX_PATH];
    int storage_root_size;
    pxa_status_t status;
    if (!c_host_app_data_path(data_root, sizeof(data_root), app))
        return 0;
    storage_root_size = snprintf(storage_root, sizeof(storage_root), "%s/.pxa-storage", data_root);
    if (storage_root_size <= 0 || (size_t)storage_root_size >= sizeof(storage_root)) {
        return 0;
    }
    pxa_posix_fs_deinit(g_host.posix_fs);
    pxa_posix_storage_deinit(g_host.posix_storage);
    if (!make_dirs(data_root) || !make_dirs(storage_root))
        return 0;
    memset(&fs_config, 0, sizeof(fs_config));
    fs_config.struct_size = sizeof(fs_config);
    fs_config.root_path = data_root;
    fs_config.quota_bytes = 4ull * 1024 * 1024;
    fs_config.max_open_resources = 16;
    status = pxa_posix_fs_init(g_host.fs_service_workspace, pxa_posix_fs_workspace_size(&fs_config),
                               &fs_config, &g_host.posix_fs, &g_host.fs_backend);
    if (status != PXA_STATUS_OK)
        return 0;
    memset(&storage_config, 0, sizeof(storage_config));
    storage_config.struct_size = sizeof(storage_config);
    storage_config.root_path = storage_root;
    storage_config.max_keys = 32;
    storage_config.max_value_bytes = 960;
    storage_config.quota_bytes = 8192;
    status = pxa_posix_storage_init(
        g_host.storage_workspace, pxa_posix_storage_workspace_size(&storage_config),
        &storage_config, &g_host.posix_storage, &g_host.storage_backend);
    if (status != PXA_STATUS_OK)
        return 0;
    status = pxa_scheduler_rebind(g_host.scheduler_service, app->job_components,
                                  app->job_component_count);
    if (status == PXA_STATUS_INTERNAL) {
        /* An updated Package may no longer declare a Job referenced by its
         * persisted schedule. Drop only that incompatible schedule. */
        status = g_host.scheduler_store.save(g_host.scheduler_store.context, NULL, 0);
        if (status == PXA_STATUS_OK) {
            status = pxa_scheduler_rebind(g_host.scheduler_service, app->job_components,
                                          app->job_component_count);
        }
    }
    if (status != PXA_STATUS_OK)
        return 0;
    memset(g_host.net_operations, 0, sizeof(g_host.net_operations));
    memset(g_host.audio_sessions, 0, sizeof(g_host.audio_sessions));
    if (g_host.audio_device != 0) {
        SDL_ClearQueuedAudio(g_host.audio_device);
    }
    return 1;
}

static pxa_status_t read_artifact(void* context, pxa_bytes_t path, uint8_t* output, size_t capacity,
                                  size_t* size) {
    char *copy;
    struct stat metadata;
    int fd;
    ssize_t count;
    (void)context;
    if (size == NULL || path.data == NULL || path.size == 0 ||
        (output == NULL && capacity != 0)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    *size = 0;
    copy = (char *)malloc(path.size + 1);
    if (copy == NULL) return PXA_STATUS_INTERNAL;
    memcpy(copy, path.data, path.size);
    copy[path.size] = '\0';
    if (stat(copy, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        free(copy);
        return PXA_STATUS_NOT_FOUND;
    }
    if (metadata.st_size <= 0) {
        free(copy);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (output == NULL) {
        free(copy);
        *size = (size_t)metadata.st_size;
        return PXA_STATUS_OK;
    }
    if ((uint64_t)metadata.st_size > capacity) {
        free(copy);
        return PXA_STATUS_QUOTA_EXCEEDED;
    }
    fd = open(copy, O_RDONLY);
    free(copy);
    if (fd < 0) return PXA_STATUS_INTERNAL;
    {
        size_t offset = 0;
        while (offset < (size_t)metadata.st_size) {
            do {
                count = read(fd, output + offset,
                             (size_t)metadata.st_size - offset);
            } while (count < 0 && errno == EINTR);
            if (count <= 0) {
                close(fd);
                return PXA_STATUS_INTERNAL;
            }
            offset += (size_t)count;
        }
    }
    close(fd);
    *size = (size_t)metadata.st_size;
    return PXA_STATUS_OK;
}

/* --- window backend ----------------------------------------------------- */

static pxa_status_t c_host_window_apply(
    void *context, const pxa_window_configuration_t *configuration) {
    (void)context;
    if (configuration == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (g_system_window_changed != NULL)
        g_system_window_changed(g_system_callback_context, configuration);
    return PXA_STATUS_OK;
}

/* --- permission store: in-memory, keyed by the active package ------------ */

static int c_host_bytes_equal(pxa_bytes_t left, pxa_bytes_t right) {
    return left.size == right.size &&
           (left.size == 0 ||
            (left.data != NULL && right.data != NULL &&
             memcmp(left.data, right.data, left.size) == 0));
}

static pxa_c_host_app_t *c_host_find_app(const char *id) {
    size_t index;
    if (id == NULL) return NULL;
    for (index = 0; index < g_host.app_count; ++index) {
        if (strcmp(g_host.apps[index].id, id) == 0) return &g_host.apps[index];
    }
    return NULL;
}

static c_host_app_permission_t *c_host_find_app_permission(
    pxa_c_host_app_t *app, pxa_bytes_t name, pxa_bytes_t scope) {
    uint16_t index;
    if (app == NULL) return NULL;
    for (index = 0; index < app->permission_count; ++index) {
        c_host_app_permission_t *permission = &app->permissions[index];
        const pxa_permission_declaration_t *declaration;
        if (permission->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[permission->declaration_index];
        if (c_host_bytes_equal(name, declaration->name) &&
            c_host_bytes_equal(scope, declaration->scope)) {
            return permission;
        }
    }
    return NULL;
}

static int c_host_app_permissions_can_activate(const pxa_c_host_app_t *app) {
    uint16_t index;
    if (app == NULL) return 0;
    for (index = 0; index < app->permission_count; ++index) {
        const c_host_app_permission_t *permission = &app->permissions[index];
        if (permission->required && !permission->granted) return 0;
    }
    return 1;
}

static int c_host_hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int c_host_hex_scope_matches(const char *encoded, pxa_bytes_t scope) {
    size_t index;
    if (encoded == NULL || strlen(encoded) != scope.size * 2) return 0;
    for (index = 0; index < scope.size; ++index) {
        const int high = c_host_hex_digit(encoded[index * 2]);
        const int low = c_host_hex_digit(encoded[index * 2 + 1]);
        if (high < 0 || low < 0 || scope.data == NULL ||
            scope.data[index] != (uint8_t)((high << 4) | low)) {
            return 0;
        }
    }
    return 1;
}

static void c_host_permission_policy_path(char *output, size_t capacity,
                                          const pxa_c_host_app_t *app) {
    if (output == NULL || capacity == 0) return;
    output[0] = '\0';
    if (app == NULL) return;
    if (snprintf(output, capacity, "%s/policy/permissions/%s",
                 g_host.storage_root, app->id) >= (int)capacity) {
        output[0] = '\0';
    }
}

static void c_host_load_app_permission_policy(pxa_c_host_app_t *app) {
    char path[PXA_C_HOST_MAX_PATH];
    char line[PXA_C_HOST_PERMISSION_NAME_MAX +
              PXA_C_HOST_PERMISSION_SCOPE_MAX * 2 + 8];
    FILE *input;
    uint16_t index;
    if (app == NULL) return;
    for (index = 0; index < app->permission_count; ++index) {
        app->permissions[index].granted = 0;
    }
    c_host_permission_policy_path(path, sizeof(path), app);
    if (path[0] == '\0') return;
    input = fopen(path, "r");
    if (input == NULL) return;
    while (fgets(line, sizeof(line), input) != NULL) {
        char *scope_text;
        char *decision_text;
        size_t name_size;
        line[strcspn(line, "\r\n")] = '\0';
        scope_text = strchr(line, '\t');
        if (scope_text == NULL) continue;
        *scope_text++ = '\0';
        decision_text = strchr(scope_text, '\t');
        if (decision_text == NULL || decision_text[1] == '\0' ||
            decision_text[2] != '\0' ||
            (decision_text[1] != '0' && decision_text[1] != '1')) {
            continue;
        }
        *decision_text++ = '\0';
        name_size = strlen(line);
        for (index = 0; index < app->permission_count; ++index) {
            c_host_app_permission_t *permission = &app->permissions[index];
            const pxa_permission_declaration_t *declaration;
            if (permission->declaration_index >= g_host.declaration_count) {
                continue;
            }
            declaration = &g_host.declarations[permission->declaration_index];
            if (declaration->name.size == name_size &&
                memcmp(declaration->name.data, line, name_size) == 0 &&
                c_host_hex_scope_matches(scope_text, declaration->scope)) {
                permission->granted = decision_text[0] == '1';
                break;
            }
        }
    }
    fclose(input);
}

static pxa_status_t c_host_save_app_permission_policy(
    const pxa_c_host_app_t *app) {
    char directory[PXA_C_HOST_MAX_PATH];
    char path[PXA_C_HOST_MAX_PATH];
    char temporary[PXA_C_HOST_MAX_PATH];
    FILE *output;
    uint16_t index;
    if (app == NULL ||
        snprintf(directory, sizeof(directory), "%s/policy/permissions",
                 g_host.storage_root) >= (int)sizeof(directory)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    if (!make_dirs(directory)) return PXA_STATUS_INTERNAL;
    c_host_permission_policy_path(path, sizeof(path), app);
    if (path[0] == '\0' ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
            (int)sizeof(temporary)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    output = fopen(temporary, "w");
    if (output == NULL) return PXA_STATUS_INTERNAL;
    for (index = 0; index < app->permission_count; ++index) {
        const c_host_app_permission_t *permission = &app->permissions[index];
        const pxa_permission_declaration_t *declaration;
        size_t byte_index;
        if (permission->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[permission->declaration_index];
        if (fwrite(declaration->name.data, 1, declaration->name.size, output) !=
                declaration->name.size ||
            fputc('\t', output) == EOF) {
            fclose(output);
            unlink(temporary);
            return PXA_STATUS_INTERNAL;
        }
        for (byte_index = 0; byte_index < declaration->scope.size; ++byte_index) {
            if (fprintf(output, "%02x", declaration->scope.data[byte_index]) <
                0) {
                fclose(output);
                unlink(temporary);
                return PXA_STATUS_INTERNAL;
            }
        }
        if (fprintf(output, "\t%c\n", permission->granted ? '1' : '0') < 0) {
            fclose(output);
            unlink(temporary);
            return PXA_STATUS_INTERNAL;
        }
    }
    if (fclose(output) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        return PXA_STATUS_INTERNAL;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_permission_load(void *context,
                                           pxa_bytes_t app_identity,
                                           pxa_bytes_t name, pxa_bytes_t scope,
                                           pxa_permission_decision_t *decision) {
    c_host_app_permission_t *permission;
    (void)context;
    (void)app_identity;
    if (decision == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    *decision = PXA_PERMISSION_DENY;
    permission = c_host_find_app_permission(g_host.active_app, name, scope);
    if (permission != NULL && permission->granted) {
        *decision = PXA_PERMISSION_ALLOW;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_permission_save(void *context,
                                           pxa_bytes_t app_identity,
                                           pxa_bytes_t name, pxa_bytes_t scope,
                                           pxa_permission_decision_t decision) {
    c_host_app_permission_t *permission;
    (void)context;
    (void)app_identity;
    if (decision != PXA_PERMISSION_DENY && decision != PXA_PERMISSION_ALLOW) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    permission = c_host_find_app_permission(g_host.active_app, name, scope);
    if (permission == NULL) return PXA_STATUS_NOT_FOUND;
    {
        const uint8_t previous = permission->granted;
        permission->granted = decision == PXA_PERMISSION_ALLOW;
        if (c_host_save_app_permission_policy(g_host.active_app) !=
            PXA_STATUS_OK) {
            permission->granted = previous;
            return PXA_STATUS_INTERNAL;
        }
    }
    return PXA_STATUS_OK;
}

/* --- deterministic sensor backend -------------------------------------- */

static pxa_status_t c_host_sensor_subscribe(void *context,
                                            uint16_t descriptor_id,
                                            uint32_t period_ms,
                                            void **subscription) {
    (void)context;
    (void)period_ms;
    if (subscription == NULL || (descriptor_id != 1 && descriptor_id != 2)) {
        return PXA_STATUS_NOT_FOUND;
    }
    *subscription = (void *)(uintptr_t)descriptor_id;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_sensor_read(void *context, void *subscription,
                                       uint16_t descriptor_id,
                                       int32_t values[PXA_SENSOR_MAX_DIMENSIONS]) {
    (void)context;
    if (values == NULL || (uintptr_t)subscription != descriptor_id ||
        (descriptor_id != 1 && descriptor_id != 2)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    values[0] = descriptor_id == 1 ? 25000 : 420000;
    return PXA_STATUS_OK;
}

static void c_host_sensor_unsubscribe(void *context, void *subscription,
                                      uint16_t descriptor_id) {
    (void)context;
    (void)subscription;
    (void)descriptor_id;
}

static pxa_status_t c_host_device_get_mac(void *context, uint16_t kind,
                                          uint8_t output[6], uint32_t *flags) {
    static const uint8_t wifi_station_mac[6] =
        {0x24u, 0x6fu, 0x28u, 0x70u, 0x14u, 0x01u};
    (void)context;
    if (output == NULL || flags == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (kind != PXA_DEVICE_MAC_KIND_WIFI_STATION_HARDWARE) {
        return PXA_STATUS_NOT_FOUND;
    }
    memcpy(output, wifi_station_mac, sizeof(wifi_station_mac));
    *flags = PXA_DEVICE_MAC_FLAG_HARDWARE;
    return PXA_STATUS_OK;
}

/* --- net backend: real asynchronous HTTP transport ---------------------- */

static c_host_net_operation_t *c_host_net_find_operation(
    pxa_c_host_t *host, uint64_t operation) {
    size_t index;
    if (host == NULL || operation == 0) return NULL;
    for (index = 0; index < PXA_C_HOST_NET_MAX_OPERATIONS; ++index) {
        if (host->net_operations[index].active &&
            host->net_operations[index].operation == operation) {
            return &host->net_operations[index];
        }
    }
    return NULL;
}

static c_host_net_operation_t *c_host_net_stream(
    pxa_c_host_t *host, void *stream) {
    size_t index;
    if (host == NULL || stream == NULL) return NULL;
    for (index = 0; index < PXA_C_HOST_NET_MAX_OPERATIONS; ++index) {
        if (&host->net_operations[index] == stream &&
            host->net_operations[index].active) {
            return &host->net_operations[index];
        }
    }
    return NULL;
}

static void c_host_net_dispose_operation(pxa_c_host_t *host,
                                         c_host_net_operation_t *slot) {
    if (slot == NULL) return;
    if (slot->easy != NULL) {
        if (host != NULL && host->curl_multi != NULL)
            (void)curl_multi_remove_handle(host->curl_multi, slot->easy);
        curl_easy_cleanup(slot->easy);
    }
    curl_slist_free_all(slot->request_headers);
    memset(slot, 0, sizeof(*slot));
}

static size_t c_host_net_write_body(char *data, size_t size, size_t count,
                                    void *context) {
    c_host_net_operation_t *slot = (c_host_net_operation_t *)context;
    size_t bytes;
    if (slot == NULL || (size != 0 && count > SIZE_MAX / size)) return 0;
    bytes = size * count;
    if (bytes > slot->body_capacity - slot->body_size) {
        slot->body_limit_exceeded = 1;
        return 0;
    }
    if (bytes != 0) memcpy(slot->body + slot->body_size, data, bytes);
    slot->body_size += bytes;
    return bytes;
}

static size_t c_host_net_capture_header(char *data, size_t size, size_t count,
                                        void *context) {
    c_host_net_operation_t *slot = (c_host_net_operation_t *)context;
    size_t bytes;
    size_t name_size;
    size_t value_start;
    size_t value_end;
    size_t index;
    if (slot == NULL || (size != 0 && count > SIZE_MAX / size)) return 0;
    bytes = size * count;
    if (bytes == 0) return 0;
    for (name_size = 0; name_size < bytes && data[name_size] != ':';
         ++name_size) {
    }
    if (name_size == 0 || name_size == bytes ||
        name_size > PXA_NET_MAX_HEADER_NAME_BYTES)
        return bytes;
    value_start = name_size + 1u;
    while (value_start < bytes &&
           (data[value_start] == ' ' || data[value_start] == '\t'))
        ++value_start;
    value_end = bytes;
    while (value_end > value_start &&
           (data[value_end - 1u] == '\r' || data[value_end - 1u] == '\n' ||
            data[value_end - 1u] == ' ' || data[value_end - 1u] == '\t'))
        --value_end;
    if (value_end - value_start > PXA_NET_MAX_HEADER_VALUE_BYTES)
        return bytes;
    for (index = 0; index < slot->wanted_header_count; ++index) {
        const char *wanted = slot->wanted_header_names[index];
        uint16_t output_index;
        if (strlen(wanted) != name_size ||
            strncasecmp(wanted, data, name_size) != 0)
            continue;
        for (output_index = 0; output_index < slot->response_header_count;
             ++output_index) {
            if (strcmp((const char *)slot->response_headers[output_index].name.data,
                       wanted) == 0)
                return bytes;
        }
        output_index = slot->response_header_count;
        memcpy(slot->response_header_values[output_index], data + value_start,
               value_end - value_start);
        slot->response_header_values[output_index][value_end - value_start] = '\0';
        slot->response_headers[output_index].name.data = (const uint8_t *)wanted;
        slot->response_headers[output_index].name.size = name_size;
        slot->response_headers[output_index].value.data =
            (const uint8_t *)slot->response_header_values[output_index];
        slot->response_headers[output_index].value.size = value_end - value_start;
        ++slot->response_header_count;
        break;
    }
    return bytes;
}

static pxa_status_t c_host_net_curl_status(const c_host_net_operation_t *slot,
                                            CURLcode code) {
    if (code == CURLE_OK) return PXA_STATUS_OK;
    if (slot != NULL && slot->body_limit_exceeded) return PXA_STATUS_LIMIT_EXCEEDED;
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT: return PXA_STATUS_TIMED_OUT;
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY: return PXA_STATUS_UNAVAILABLE;
        case CURLE_OUT_OF_MEMORY: return PXA_STATUS_RESOURCE_LIMIT;
        default: return PXA_STATUS_IO_ERROR;
    }
}

static const char *c_host_net_method_name(uint16_t method) {
    switch (method) {
        case PXA_NET_METHOD_GET: return "GET";
        case PXA_NET_METHOD_HEAD: return "HEAD";
        case PXA_NET_METHOD_POST: return "POST";
        case PXA_NET_METHOD_PUT: return "PUT";
        case PXA_NET_METHOD_PATCH: return "PATCH";
        case PXA_NET_METHOD_DELETE: return "DELETE";
        default: return NULL;
    }
}

static void c_host_net_progress(pxa_c_host_t *host) {
    CURLMsg *message;
    int running = 0;
    int messages = 0;
    if (host == NULL || host->curl_multi == NULL) return;
    while (curl_multi_perform(host->curl_multi, &running) == CURLM_CALL_MULTI_PERFORM) {
    }
    while ((message = curl_multi_info_read(host->curl_multi, &messages)) != NULL) {
        c_host_net_operation_t *slot = NULL;
        char *private_data = NULL;
        long status_code = 0;
        char *content_type = NULL;
        if (message->msg != CURLMSG_DONE) continue;
        if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE,
                              &private_data) != CURLE_OK)
            continue;
        slot = (c_host_net_operation_t *)private_data;
        if (slot == NULL || !slot->active || slot->easy != message->easy_handle)
            continue;
        (void)curl_easy_getinfo(slot->easy, CURLINFO_RESPONSE_CODE, &status_code);
        if (status_code >= 100 && status_code <= 999)
            slot->status_code = (uint16_t)status_code;
        if (curl_easy_getinfo(slot->easy, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK &&
            content_type != NULL) {
            slot->content_type_size = strnlen(content_type, PXA_NET_MAX_CONTENT_TYPE_BYTES);
            memcpy(slot->content_type, content_type, slot->content_type_size);
            slot->content_type[slot->content_type_size] = '\0';
        }
        slot->result = c_host_net_curl_status(slot, message->data.result);
        slot->complete = 1;
        (void)curl_multi_remove_handle(host->curl_multi, slot->easy);
    }
}

static pxa_status_t c_host_net_start(void *context,
                                     const pxa_net_request_t *request,
                                     uint64_t *operation) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    size_t index;
    CURLcode code;
    CURLMcode multi_code;
    const char *method;
    if (host == NULL || request == NULL || operation == NULL ||
        request->struct_size < sizeof(*request) ||
        request->method < PXA_NET_METHOD_GET ||
        request->method > PXA_NET_METHOD_DELETE || request->url.data == NULL ||
        request->url.size == 0 || request->url.size > PXA_NET_MAX_URL_BYTES ||
        request->body.size > (size_t)LONG_MAX || request->timeout_ms > (uint32_t)LONG_MAX ||
        request->max_response_bytes > sizeof(((c_host_net_operation_t *)0)->body) ||
        request->header_count > PXA_NET_MAX_HEADERS ||
        request->wanted_response_header_count > PXA_NET_MAX_HEADERS ||
        (request->header_count != 0 && request->headers == NULL) ||
        (request->wanted_response_header_count != 0 &&
         request->wanted_response_headers == NULL) ||
        (request->body.size != 0 && request->body.data == NULL)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    method = c_host_net_method_name(request->method);
    if (method == NULL || host->curl_multi == NULL) return PXA_STATUS_BAD_STATE;
    for (index = 0; index < PXA_C_HOST_NET_MAX_OPERATIONS; ++index) {
        c_host_net_operation_t *slot = &host->net_operations[index];
        if (slot->active) continue;
        do {
            ++host->next_net_operation;
        } while (host->next_net_operation == 0 ||
                 c_host_net_find_operation(host,
                                           host->next_net_operation) != NULL);
        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        slot->operation = host->next_net_operation;
        slot->method = request->method;
        slot->abi_minor = request->abi_minor;
        slot->timeout_ms = request->timeout_ms;
        slot->body_capacity = request->max_response_bytes;
        slot->wanted_header_count = request->wanted_response_header_count;
        memcpy(slot->url, request->url.data, request->url.size);
        slot->url[request->url.size] = '\0';
        for (size_t header_index = 0;
             header_index < request->wanted_response_header_count; ++header_index) {
            size_t name_size = request->wanted_response_headers[header_index].size;
            if (name_size == 0 || name_size > PXA_NET_MAX_HEADER_NAME_BYTES) {
                c_host_net_dispose_operation(host, slot);
                return PXA_STATUS_INVALID_ARGUMENT;
            }
            memcpy(slot->wanted_header_names[header_index],
                   request->wanted_response_headers[header_index].data, name_size);
            slot->wanted_header_names[header_index][name_size] = '\0';
            slot->response_headers[header_index].name.data =
                (const uint8_t *)slot->wanted_header_names[header_index];
            slot->response_headers[header_index].name.size = name_size;
        }
        slot->easy = curl_easy_init();
        if (slot->easy == NULL) {
            c_host_net_dispose_operation(host, slot);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        for (size_t header_index = 0; header_index < request->header_count;
             ++header_index) {
            char header[PXA_NET_MAX_HEADER_NAME_BYTES + 2u +
                        PXA_NET_MAX_HEADER_VALUE_BYTES + 1u];
            size_t name_size = request->headers[header_index].name.size;
            size_t value_size = request->headers[header_index].value.size;
            if (name_size > PXA_NET_MAX_HEADER_NAME_BYTES ||
                value_size > PXA_NET_MAX_HEADER_VALUE_BYTES) {
                c_host_net_dispose_operation(host, slot);
                return PXA_STATUS_INVALID_ARGUMENT;
            }
            memcpy(header, request->headers[header_index].name.data, name_size);
            header[name_size] = ':';
            header[name_size + 1u] = ' ';
            memcpy(header + name_size + 2u,
                   request->headers[header_index].value.data, value_size);
            header[name_size + 2u + value_size] = '\0';
            {
                struct curl_slist *headers =
                    curl_slist_append(slot->request_headers, header);
                if (headers == NULL) {
                    c_host_net_dispose_operation(host, slot);
                    return PXA_STATUS_RESOURCE_LIMIT;
                }
                slot->request_headers = headers;
            }
        }
        code = curl_easy_setopt(slot->easy, CURLOPT_URL, slot->url);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_PRIVATE, slot);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_WRITEFUNCTION,
                                                       c_host_net_write_body);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_WRITEDATA, slot);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_HEADERFUNCTION,
                                                       c_host_net_capture_header);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_HEADERDATA, slot);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_HTTPHEADER,
                                                       slot->request_headers);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_NOSIGNAL, 1L);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_FOLLOWLOCATION, 0L);
        if (code == CURLE_OK) code = curl_easy_setopt(slot->easy, CURLOPT_TIMEOUT_MS,
                                                       (long)slot->timeout_ms);
        if (request->method == PXA_NET_METHOD_HEAD && code == CURLE_OK)
            code = curl_easy_setopt(slot->easy, CURLOPT_NOBODY, 1L);
        else if (request->method == PXA_NET_METHOD_POST && code == CURLE_OK)
            code = curl_easy_setopt(slot->easy, CURLOPT_POST, 1L);
        else if (request->method != PXA_NET_METHOD_GET && code == CURLE_OK)
            code = curl_easy_setopt(slot->easy, CURLOPT_CUSTOMREQUEST, method);
        if (request->body.size != 0 && code == CURLE_OK) {
            code = curl_easy_setopt(slot->easy, CURLOPT_POSTFIELDSIZE,
                                    (long)request->body.size);
            if (code == CURLE_OK)
                code = curl_easy_setopt(slot->easy, CURLOPT_COPYPOSTFIELDS,
                                        (const char *)request->body.data);
        }
        if (code != CURLE_OK) {
            c_host_net_dispose_operation(host, slot);
            return PXA_STATUS_INTERNAL;
        }
        multi_code = curl_multi_add_handle(host->curl_multi, slot->easy);
        if (multi_code != CURLM_OK) {
            c_host_net_dispose_operation(host, slot);
            return PXA_STATUS_UNAVAILABLE;
        }
        *operation = slot->operation;
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_RESOURCE_LIMIT;
}

static pxa_status_t c_host_net_poll(void *context, uint64_t operation,
                                    pxa_net_response_t *response) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_net_operation_t *slot =
        c_host_net_find_operation(host, operation);
    if (slot == NULL || response == NULL) return PXA_STATUS_NOT_FOUND;
    c_host_net_progress(host);
    if (!slot->complete) return PXA_STATUS_WOULD_BLOCK;
    if (slot->result != PXA_STATUS_OK) return slot->result;
    response->status_code = slot->status_code;
    response->content_type.data = (const uint8_t *)slot->content_type;
    response->content_type.size = slot->content_type_size;
    response->body_stream = slot;
    response->headers = slot->response_headers;
    response->header_count = slot->response_header_count;
    response->flags = 0;
    response->body_length = 0;
    if (slot->abi_minor >= 1) {
        if (slot->body_size != 0) response->flags |= PXA_NET_RESPONSE_BODY_PRESENT;
        response->flags |= PXA_NET_RESPONSE_BODY_LENGTH_KNOWN;
        response->body_length = slot->body_size;
    } else {
        response->body_length = slot->body_size;
    }
    return PXA_STATUS_OK;
}

static void c_host_net_cancel(void *context, uint64_t operation) {
    c_host_net_operation_t *slot = c_host_net_find_operation(
        (pxa_c_host_t *)context, operation);
    if (slot != NULL) c_host_net_dispose_operation((pxa_c_host_t *)context, slot);
}

static pxa_status_t c_host_net_read_body(void *context, void *body_stream,
                                         uint8_t *output, size_t capacity,
                                         size_t *size) {
    c_host_net_operation_t *slot = c_host_net_stream(
        (pxa_c_host_t *)context, body_stream);
    size_t remaining;
    size_t count;
    if (size == NULL || (output == NULL && capacity != 0)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    *size = 0;
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (!slot->complete || slot->result != PXA_STATUS_OK) return PXA_STATUS_BAD_STATE;
    if (slot->body_offset > slot->body_size) return PXA_STATUS_INTERNAL;
    remaining = slot->body_size - slot->body_offset;
    count = remaining < capacity ? remaining : capacity;
    if (count != 0) {
        memcpy(output, slot->body + slot->body_offset, count);
        slot->body_offset += count;
    }
    *size = count;
    return PXA_STATUS_OK;
}

static void c_host_net_close_body(void *context, void *body_stream) {
    c_host_net_operation_t *slot = c_host_net_stream(
        (pxa_c_host_t *)context, body_stream);
    if (slot != NULL) c_host_net_dispose_operation((pxa_c_host_t *)context, slot);
}

/* --- surface backend: LVGL-presented RGB565 surface -------------------- */

static void c_host_surface_fragment_deleted(lv_event_t *event) {
    c_host_surface_fragment_t *fragment =
        (c_host_surface_fragment_t *)lv_event_get_user_data(event);
    if (fragment != NULL) fragment->object = NULL;
}

static void c_host_surface_delete_fragment_objects(
    c_host_surface_t *surface) {
    uint8_t index;
    if (surface == NULL) return;
    for (index = 0; index < surface->fragment_count; ++index) {
        if (surface->fragments[index].object != NULL)
            lv_obj_delete(surface->fragments[index].object);
    }
}

static bool c_host_surface_append_fragment(
    c_host_surface_fragment_t *fragments, uint8_t *fragment_count,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    c_host_surface_fragment_t *fragment;
    if (width == 0 || height == 0) return true;
    if (*fragment_count >= C_HOST_SURFACE_MAX_FRAGMENTS) return false;
    fragment = &fragments[(*fragment_count)++];
    memset(fragment, 0, sizeof(*fragment));
    fragment->x = x;
    fragment->y = y;
    fragment->width = width;
    fragment->height = height;
    return true;
}

/* Split at every overlay Y edge, then fill the exposed X intervals of each
 * strip. This handles overlapping rectangles. Eight overlay regions create at
 * most 17 strips with nine visible intervals each, without RGB565 copies. */
static pxa_status_t c_host_surface_set_fragments(
    c_host_surface_t *surface,
    const pxa_surface_damage_rect_t *regions, uint8_t region_count) {
    c_host_surface_fragment_t fragments[C_HOST_SURFACE_MAX_FRAGMENTS];
    uint16_t y_edges[2u + 2u * PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    uint16_t interval_start[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    uint16_t interval_end[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS];
    uint8_t fragment_count = 0;
    uint8_t y_edge_count = 2;
    uint8_t region_index;
    uint8_t y_index;

    y_edges[0] = 0;
    y_edges[1] = surface->display_height;
    for (region_index = 0; region_index < region_count; ++region_index) {
        y_edges[y_edge_count++] = regions[region_index].y;
        y_edges[y_edge_count++] =
            (uint16_t)(regions[region_index].y + regions[region_index].height);
    }
    for (y_index = 1; y_index < y_edge_count; ++y_index) {
        uint8_t cursor = y_index;
        const uint16_t value = y_edges[y_index];
        while (cursor != 0 && y_edges[cursor - 1u] > value) {
            y_edges[cursor] = y_edges[cursor - 1u];
            --cursor;
        }
        y_edges[cursor] = value;
    }
    {
        uint8_t unique_count = 1;
        for (y_index = 1; y_index < y_edge_count; ++y_index) {
            if (y_edges[y_index] != y_edges[unique_count - 1u])
                y_edges[unique_count++] = y_edges[y_index];
        }
        y_edge_count = unique_count;
    }
    for (y_index = 0; y_index + 1u < y_edge_count; ++y_index) {
        const uint16_t top = y_edges[y_index];
        const uint16_t bottom = y_edges[y_index + 1u];
        uint8_t interval_count = 0;
        uint8_t interval_index;
        uint16_t visible_x = 0;
        for (region_index = 0; region_index < region_count; ++region_index) {
            const pxa_surface_damage_rect_t *region = &regions[region_index];
            const uint16_t region_bottom =
                (uint16_t)(region->y + region->height);
            if (region->y > top || region_bottom < bottom) continue;
            interval_start[interval_count] = region->x;
            interval_end[interval_count++] =
                (uint16_t)(region->x + region->width);
        }
        for (interval_index = 1; interval_index < interval_count;
             ++interval_index) {
            const uint16_t start = interval_start[interval_index];
            const uint16_t end = interval_end[interval_index];
            uint8_t cursor = interval_index;
            while (cursor != 0 && interval_start[cursor - 1u] > start) {
                interval_start[cursor] = interval_start[cursor - 1u];
                interval_end[cursor] = interval_end[cursor - 1u];
                --cursor;
            }
            interval_start[cursor] = start;
            interval_end[cursor] = end;
        }
        for (interval_index = 0; interval_index < interval_count;
             ++interval_index) {
            if (interval_start[interval_index] > visible_x &&
                !c_host_surface_append_fragment(
                    fragments, &fragment_count, visible_x, top,
                    (uint16_t)(interval_start[interval_index] - visible_x),
                    (uint16_t)(bottom - top))) {
                return PXA_STATUS_RESOURCE_LIMIT;
            }
            if (interval_end[interval_index] > visible_x)
                visible_x = interval_end[interval_index];
        }
        if (visible_x < surface->display_width && !c_host_surface_append_fragment(
                fragments, &fragment_count, visible_x, top,
                (uint16_t)(surface->display_width - visible_x),
                (uint16_t)(bottom - top))) {
            return PXA_STATUS_RESOURCE_LIMIT;
        }
    }
    c_host_surface_delete_fragment_objects(surface);
    memcpy(surface->fragments, fragments, sizeof(fragments));
    surface->fragment_count = fragment_count;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_present(
    c_host_surface_t *surface) {
    uint8_t index;
    const int mapped = surface != NULL &&
        (surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0;
    const int raster = surface != NULL &&
        (surface->flags & PXA_APP_PAGES_GAME_CONTEXT) != 0;
    if (surface == NULL || ((mapped || raster) ? !surface->has_display_frame :
                            surface->current < 0))
        return PXA_STATUS_BAD_STATE;
    memset(&surface->image, 0, sizeof(surface->image));
    surface->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    surface->image.header.cf =
        surface->format == PXA_SURFACE_FORMAT_RGB565
            ? LV_COLOR_FORMAT_RGB565
            : LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED;
    surface->image.header.w = surface->display_width;
    surface->image.header.h = surface->display_height;
    surface->image.header.stride = (uint16_t)surface->display_stride_bytes;
    surface->image.data_size = surface->display_frame_bytes;
    surface->image.data = (mapped || raster)
                              ? surface->display_buffer
                              : surface->buffers[surface->current];
    for (index = 0; index < surface->fragment_count; ++index) {
        c_host_surface_fragment_t *fragment = &surface->fragments[index];
        if (fragment->object == NULL) {
            fragment->object = lv_image_create(lv_screen_active());
            if (fragment->object == NULL) return PXA_STATUS_RESOURCE_LIMIT;
            lv_obj_add_event_cb(fragment->object,
                                c_host_surface_fragment_deleted,
                                LV_EVENT_DELETE, fragment);
            lv_obj_clear_flag(fragment->object, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(fragment->object, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(fragment->object, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
            lv_image_set_inner_align(fragment->object, LV_IMAGE_ALIGN_TOP_LEFT);
        }
        lv_image_set_src(fragment->object, &surface->image);
        lv_obj_set_size(fragment->object, fragment->width, fragment->height);
        lv_obj_set_pos(fragment->object,
                       surface->layer.x + fragment->x,
                       surface->layer.y + fragment->y);
        lv_image_set_offset_x(fragment->object, -(int32_t)fragment->x);
        lv_image_set_offset_y(fragment->object, -(int32_t)fragment->y);
        if (surface->layer.visible)
            lv_obj_remove_flag(fragment->object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(fragment->object, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(fragment->object);
        lv_obj_invalidate(fragment->object);
    }
    {
        pxa_lvgl_ui_alpha_plane_t plane;
        if (pxa_lvgl_ui_alpha_plane(g_host.ui_adapter, &plane)) {
            const size_t size = (size_t)plane.width * plane.height * 4u;
            if (size > surface->ui_alpha_capacity) {
                uint8_t *bytes = realloc(surface->ui_alpha_bytes, size);
                if (bytes == NULL) return PXA_STATUS_RESOURCE_LIMIT;
                surface->ui_alpha_bytes = bytes;
                surface->ui_alpha_capacity = size;
            }
            for (uint32_t y = 0; y < plane.height; ++y) {
                const uint16_t *colors = (const uint16_t *)(
                    (const uint8_t *)plane.pixels +
                    (size_t)y * plane.pixel_stride_bytes);
                const uint8_t *alpha = plane.alpha +
                    (size_t)y * plane.alpha_stride_bytes;
                lv_color32_t *output = (lv_color32_t *)(
                    surface->ui_alpha_bytes +
                    (size_t)y * plane.width * sizeof(lv_color32_t));
                for (uint32_t x = 0; x < plane.width; ++x) {
                    const uint8_t a = alpha[x];
                    output[x].red = (uint8_t)(((uint16_t)
                        ((colors[x] >> 11) & 0x1fu) * 255u / 31u * a +
                        127u) / 255u);
                    output[x].green = (uint8_t)(((uint16_t)
                        ((colors[x] >> 5) & 0x3fu) * 255u / 63u * a +
                        127u) / 255u);
                    output[x].blue = (uint8_t)(((uint16_t)(colors[x] & 0x1fu) *
                        255u / 31u * a + 127u) / 255u);
                    output[x].alpha = a;
                }
            }
            if (surface->ui_alpha_object == NULL) {
                surface->ui_alpha_object = lv_image_create(lv_screen_active());
                if (surface->ui_alpha_object == NULL)
                    return PXA_STATUS_RESOURCE_LIMIT;
                lv_obj_clear_flag(surface->ui_alpha_object,
                                  LV_OBJ_FLAG_SCROLLABLE);
                lv_image_set_inner_align(surface->ui_alpha_object,
                                         LV_IMAGE_ALIGN_TOP_LEFT);
            }
            memset(&surface->ui_alpha_image, 0, sizeof(surface->ui_alpha_image));
            surface->ui_alpha_image.header.magic = LV_IMAGE_HEADER_MAGIC;
            surface->ui_alpha_image.header.cf =
                LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED;
            surface->ui_alpha_image.header.w = plane.width;
            surface->ui_alpha_image.header.h = plane.height;
            surface->ui_alpha_image.header.stride = (uint16_t)plane.width * 4u;
            surface->ui_alpha_image.data_size = size;
            surface->ui_alpha_image.data = surface->ui_alpha_bytes;
            lv_image_set_src(surface->ui_alpha_object, &surface->ui_alpha_image);
            lv_obj_set_size(surface->ui_alpha_object, plane.width, plane.height);
            lv_obj_set_pos(surface->ui_alpha_object, plane.x, plane.y);
            lv_obj_remove_flag(surface->ui_alpha_object, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(surface->ui_alpha_object);
            lv_obj_invalidate(surface->ui_alpha_object);
        } else if (surface->ui_alpha_object != NULL) {
            lv_obj_add_flag(surface->ui_alpha_object, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_create_impl(
    void *context, const pxa_surface_desc_t *desc,
    uint64_t *provider_surface, uint32_t *stride_bytes, uint32_t draw_capacity) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface;
    uint8_t bytes_per_pixel;
    uint8_t index;
    uint8_t scale = 1;
    const int mapped = desc != NULL &&
        (desc->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0;
    const int raster = desc != NULL &&
        (desc->flags & PXA_APP_PAGES_GAME_CONTEXT) != 0;
    if (host == NULL || desc == NULL || provider_surface == NULL ||
        stride_bytes == NULL || host->surface != NULL ||
        desc->width == 0 || desc->height == 0 || desc->buffer_count < 2 ||
        desc->buffer_count > 3)
        return PXA_STATUS_INVALID_ARGUMENT;
    /* SDL has no independent panel scanout. Accept the direct preference to
     * exercise the same Guest protocol; presentation remains an LVGL image. */
    if ((desc->flags & ~(PXA_SURFACE_FLAG_KNOWN_MASK | PXA_APP_PAGES_GAME_CONTEXT)) != 0)
        return PXA_STATUS_UNSUPPORTED;
    if (mapped && raster) return PXA_STATUS_UNSUPPORTED;
    if (desc->format == PXA_SURFACE_FORMAT_RGB565 &&
        (desc->flags & PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA) == 0)
        bytes_per_pixel = 2;
    else if (desc->format == PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED &&
             desc->flags == PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA)
        bytes_per_pixel = 4;
    else
        return PXA_STATUS_UNSUPPORTED;
    surface = (c_host_surface_t *)calloc(1, sizeof(*surface));
    if (surface == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    if (mapped || raster) {
        if (desc->format != PXA_SURFACE_FORMAT_RGB565 ||
            g_display_metrics.width % desc->width != 0 ||
            g_display_metrics.height % desc->height != 0 ||
            g_display_metrics.width / desc->width !=
                g_display_metrics.height / desc->height) {
            free(surface);
            return PXA_STATUS_UNSUPPORTED;
        }
        scale = (uint8_t)(g_display_metrics.width / desc->width);
        if (scale != 1 && scale != 2 && scale != 3 && scale != 4) {
            free(surface);
            return PXA_STATUS_UNSUPPORTED;
        }
    }
    surface->width = desc->width;
    surface->height = desc->height;
    surface->display_width = (mapped || raster) ? g_display_metrics.width
                                                : desc->width;
    surface->display_height = (mapped || raster) ? g_display_metrics.height
                                                 : desc->height;
    surface->buffer_count = desc->buffer_count;
    surface->stride_bytes = (uint32_t)desc->width * bytes_per_pixel;
    surface->frame_bytes = surface->stride_bytes * desc->height;
    surface->display_stride_bytes =
        (uint32_t)surface->display_width * bytes_per_pixel;
    surface->display_frame_bytes =
        surface->display_stride_bytes * surface->display_height;
    surface->format = desc->format;
    surface->flags = desc->flags;
    surface->scale = scale;
    surface->staged = -1;
    surface->current = -1;
    surface->writing = -1;
    surface->pending = -1;
    surface->layer.width = desc->width;
    surface->layer.height = desc->height;
    if (c_host_surface_set_fragments(surface, NULL, 0) != PXA_STATUS_OK) {
        free(surface);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (mapped || raster) {
        surface->display_buffer = malloc(surface->display_frame_bytes);
        if (surface->display_buffer == NULL) {
            free(surface);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
    }
    if (!mapped) {
        for (index = 0; index < surface->buffer_count; ++index) {
            surface->buffers[index] = (uint8_t *)malloc(surface->frame_bytes);
            if (surface->buffers[index] == NULL) {
                while (index != 0) free(surface->buffers[--index]);
                free(surface->display_buffer);
                free(surface);
                return PXA_STATUS_RESOURCE_LIMIT;
            }
        }
    }
    if (raster) {
        for (index = 0; index < 2; ++index) {
            surface->raster_draw_lists[index] =
                (uint8_t *)malloc(draw_capacity);
            if (surface->raster_draw_lists[index] == NULL) {
                while (index != 0) free(surface->raster_draw_lists[--index]);
                for (index = 0; index < surface->buffer_count; ++index)
                    free(surface->buffers[index]);
                free(surface->display_buffer);
                free(surface);
                return PXA_STATUS_RESOURCE_LIMIT;
            }
        }
    }
    surface->raster_draw_pending = -1;
    surface->raster_draw_capacity = draw_capacity;
    host->surface = surface;
    *provider_surface = (uint64_t)(uintptr_t)surface;
    *stride_bytes = surface->stride_bytes;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_create(void *context, const pxa_surface_desc_t *desc,
    uint64_t *provider, uint32_t *stride) {
    return c_host_surface_create_impl(context, desc, provider, stride, 0);
}

static pxa_status_t c_host_surface_write(void *context,
                                          uint64_t provider_surface,
                                          const uint8_t *pixels,
                                          size_t size) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    int index;
    if (host == NULL || surface == NULL || surface != host->surface ||
        pixels == NULL || size != surface->frame_bytes ||
        (surface->flags & (PXA_SURFACE_FLAG_GUEST_MAPPED |
                           PXA_APP_PAGES_GAME_CONTEXT)) != 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (surface->staged >= 0) return PXA_STATUS_WOULD_BLOCK;
    index = surface->current == 0 ? 1 : 0;
    if (index >= surface->buffer_count) return PXA_STATUS_WOULD_BLOCK;
    memcpy(surface->buffers[index], pixels, size);
    surface->staged = (int8_t)index;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_queue(
    void *context, uint64_t provider_surface, uint64_t frame_id,
    const pxa_surface_damage_rect_t *damage, uint8_t damage_count) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    (void)damage;
    (void)damage_count;
    if (host == NULL || surface == NULL || surface != host->surface ||
        frame_id == 0 ||
        (surface->flags & (PXA_SURFACE_FLAG_GUEST_MAPPED |
                           PXA_APP_PAGES_GAME_CONTEXT)) != 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (surface->staged < 0) return PXA_STATUS_BAD_STATE;
    surface->current = surface->staged;
    surface->staged = -1;
    ++surface->submitted_frames;
    ++surface->presented_frames;
    return c_host_surface_present(surface);
}

static int c_host_surface_mapped_buffer_free(
    const c_host_surface_t *surface, uint8_t index) {
    return surface != NULL && index < surface->buffer_count &&
           surface->writing != (int8_t)index &&
           surface->pending != (int8_t)index &&
           (surface->release_pending_mask & (uint8_t)(1u << index)) == 0;
}

static int c_host_surface_enqueue_release(c_host_surface_t *surface,
                                          uint8_t index,
                                          uint64_t frame_id) {
    uint8_t tail;
    if (surface == NULL || index >= surface->buffer_count || frame_id == 0 ||
        surface->release_count >= surface->buffer_count ||
        (surface->release_pending_mask & (uint8_t)(1u << index)) != 0)
        return 0;
    tail = (uint8_t)((surface->release_head + surface->release_count) %
                     surface->buffer_count);
    memset(&surface->releases[tail], 0, sizeof(surface->releases[tail]));
    surface->releases[tail].buffer_index = index;
    surface->releases[tail].frame_id = frame_id;
    surface->release_pending_mask |= (uint8_t)(1u << index);
    ++surface->release_count;
    return 1;
}

static pxa_status_t c_host_surface_register_buffers(
    void *context, uint64_t provider_surface, uint8_t *pixels, size_t size) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    uintptr_t native_alignment_mask;
    uint8_t index;
    if (host == NULL || surface == NULL || surface != host->surface ||
        pixels == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    if ((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) == 0 ||
        surface->mapped_registered)
        return PXA_STATUS_BAD_STATE;
    native_alignment_mask =
        surface->format == PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED ? 3u : 1u;
    if (((uintptr_t)pixels & native_alignment_mask) != 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (size != (size_t)surface->frame_bytes * surface->buffer_count)
        return PXA_STATUS_INVALID_ARGUMENT;
    for (index = 0; index < surface->buffer_count; ++index)
        surface->mapped_buffers[index] =
            pixels + (size_t)index * surface->frame_bytes;
    surface->mapped_registered = 1;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_acquire_buffer(
    void *context, uint64_t provider_surface, uint8_t *buffer_index) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    uint8_t index;
    if (host == NULL || surface == NULL || surface != host->surface ||
        buffer_index == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!surface->mapped_registered) return PXA_STATUS_BAD_STATE;
    if (surface->writing >= 0) return PXA_STATUS_BAD_STATE;
    for (index = 0; index < surface->buffer_count; ++index) {
        if (c_host_surface_mapped_buffer_free(surface, index)) {
            surface->writing = (int8_t)index;
            *buffer_index = index;
            return PXA_STATUS_OK;
        }
    }
    return PXA_STATUS_WOULD_BLOCK;
}

static pxa_status_t c_host_surface_present_buffer(
    void *context, uint64_t provider_surface, uint8_t buffer_index,
    uint64_t frame_id) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (host == NULL || surface == NULL || surface != host->surface ||
        frame_id == 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!surface->mapped_registered || buffer_index >= surface->buffer_count ||
        surface->writing != (int8_t)buffer_index ||
        frame_id <= surface->last_frame_id)
        return PXA_STATUS_BAD_STATE;
    if (surface->pending >= 0) {
        if (!c_host_surface_enqueue_release(
                surface, (uint8_t)surface->pending,
                surface->pending_frame_id))
            return PXA_STATUS_WOULD_BLOCK;
        ++surface->dropped_frames;
        ++surface->replaced_frames;
    }
    surface->pending = (int8_t)buffer_index;
    surface->pending_frame_id = frame_id;
    surface->last_frame_id = frame_id;
    surface->writing = -1;
    ++surface->submitted_frames;
    return PXA_STATUS_OK;
}

static void c_host_raster_resources(const c_host_surface_t *surface,
                                    pxa_raster_resources_t *resources) {
    uint8_t index;
    memset(resources, 0, sizeof(*resources));
    resources->palette = surface->raster_palette;
    resources->capabilities = PXA_APP_PAGES_RASTER_CAPS;
    resources->palette_light_levels = surface->raster_palette_levels;
    for (index = 0; index < PXA_RASTER_MAX_TEXTURES; ++index) {
        resources->textures[index].pixels = surface->raster_textures[index];
        resources->textures[index].width = surface->raster_texture_width[index];
        resources->textures[index].height = surface->raster_texture_height[index];
    }
}

static pxa_status_t c_host_surface_raster_upload(
    void *context, uint64_t provider_surface, const uint8_t *bytes,
    size_t size) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    pxa_raster_upload_view_t upload;
    uint8_t *replacement;
    uint8_t *previous;
    pxa_status_t status;
    if (host == NULL || surface == NULL || surface != host->surface ||
        bytes == NULL ||
        (surface->flags & PXA_APP_PAGES_GAME_CONTEXT) == 0)
        return PXA_STATUS_BAD_STATE;
    if (surface->raster_draw_pending >= 0) return PXA_STATUS_WOULD_BLOCK;
    status = pxa_raster_decode_upload(bytes, size, &upload);
    if (status != PXA_STATUS_OK) return status;
    replacement = (uint8_t *)malloc(upload.payload_bytes);
    if (replacement == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    if (upload.kind == PXA_RASTER_UPLOAD_PALETTE_RGB565) {
        uint16_t index;
        for (index = 0; index < upload.payload_bytes / 2u; ++index)
            ((uint16_t *)replacement)[index] =
                pxa_read_u16(upload.payload + (size_t)index * 2u);
    } else {
        memcpy(replacement, upload.payload, upload.payload_bytes);
    }
    if (upload.kind == PXA_RASTER_UPLOAD_PALETTE_RGB565) {
        previous = (uint8_t *)surface->raster_palette;
        surface->raster_palette = (uint16_t *)replacement;
        surface->raster_palette_levels = upload.height ? upload.height : 1;
    } else {
        previous = surface->raster_textures[upload.slot];
        surface->raster_textures[upload.slot] = replacement;
        surface->raster_texture_width[upload.slot] = upload.width;
        surface->raster_texture_height[upload.slot] = upload.height;
    }
    free(previous);
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_raster_submit(
    void *context, uint64_t provider_surface, const uint8_t *bytes,
    size_t size) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (surface && size > surface->raster_draw_capacity) return PXA_STATUS_RESOURCE_LIMIT;
    pxa_raster_resources_t resources;
    pxa_raster_target_t target = {0};
    pxa_raster_draw_list_view_t list;
    uint64_t frequency;
    uint8_t mailbox_index;
    pxa_status_t status;
    if (host == NULL || surface == NULL || surface != host->surface ||
        bytes == NULL ||
        (surface->flags & PXA_APP_PAGES_GAME_CONTEXT) == 0)
        return PXA_STATUS_BAD_STATE;
    c_host_raster_resources(surface, &resources);
    target.pixels = (uint16_t *)surface->buffers[0];
    target.stride_pixels = surface->stride_bytes / sizeof(uint16_t);
    target.width = surface->width;
    target.height = surface->height;
    target.depth_pixels = surface->raster_depth;
    target.depth_stride_pixels = surface->width;
    target.scratch_mode = surface->raster_scratch_mode;
    status = pxa_raster_validate_draw_list(bytes, size, &target, &resources,
                                           &list);
    if (status != PXA_STATUS_OK ||
        list.frame_id <= surface->raster_last_frame_id) {
        ++surface->raster_telemetry.rejected_lists;
        return status != PXA_STATUS_OK ? status : PXA_STATUS_BAD_STATE;
    }
    frequency = (uint64_t)SDL_GetPerformanceFrequency();
    mailbox_index = surface->raster_draw_pending >= 0
                        ? (uint8_t)surface->raster_draw_pending
                        : (uint8_t)(list.frame_id & 1u);
    if (surface->raster_draw_pending >= 0) {
        ++surface->dropped_frames;
        ++surface->replaced_frames;
        ++surface->raster_telemetry.dropped_frames;
    }
    memcpy(surface->raster_draw_lists[mailbox_index], bytes, size);
    surface->raster_draw_sizes[mailbox_index] = (uint32_t)size;
    surface->raster_draw_queued_us[mailbox_index] =
        frequency == 0 ? 0
                       : (uint64_t)SDL_GetPerformanceCounter() *
                             UINT64_C(1000000) / frequency;
    surface->raster_draw_pending = (int8_t)mailbox_index;
    surface->last_frame_id = list.frame_id;
    surface->raster_last_frame_id = list.frame_id;
    ++surface->submitted_frames;
    ++surface->raster_telemetry.submitted_frames;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_raster_query(
    void *context, uint64_t provider_surface,
    pxa_raster_telemetry_t *telemetry) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (host == NULL || surface == NULL || surface != host->surface ||
        telemetry == NULL ||
        (surface->flags & PXA_APP_PAGES_GAME_CONTEXT) == 0)
        return PXA_STATUS_BAD_STATE;
    *telemetry = surface->raster_telemetry;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_peek_release(
    void *context, uint64_t provider_surface, pxa_surface_release_t *release) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (host == NULL || surface == NULL || surface != host->surface ||
        release == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (surface->release_count == 0) return PXA_STATUS_WOULD_BLOCK;
    *release = surface->releases[surface->release_head];
    return PXA_STATUS_OK;
}

static void c_host_surface_consume_release(
    void *context, uint64_t provider_surface) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    pxa_surface_release_t *release;
    if (host == NULL || surface == NULL || surface != host->surface ||
        surface->release_count == 0)
        return;
    release = &surface->releases[surface->release_head];
    surface->release_pending_mask &=
        (uint8_t)~(uint8_t)(1u << release->buffer_index);
    memset(release, 0, sizeof(*release));
    surface->release_head =
        (uint8_t)((surface->release_head + 1u) % surface->buffer_count);
    --surface->release_count;
    ++surface->released_frames;
}

static int c_host_surface_process_pending(void) {
    c_host_surface_t *surface = g_host.surface;
    uint8_t index;
    uint64_t frame_id;
    pxa_status_t status;
    const int mapped = surface != NULL &&
        (surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0;
    const int raster = surface != NULL &&
        (surface->flags & PXA_APP_PAGES_GAME_CONTEXT) != 0;
    if (raster && surface->raster_draw_pending >= 0) {
        pxa_raster_resources_t resources;
        pxa_raster_target_t target = {0};
        pxa_raster_draw_list_view_t list;
        pxa_raster_telemetry_t frame_telemetry;
        const uint8_t draw_index = (uint8_t)surface->raster_draw_pending;
        const uint8_t target_index = surface->current == 0 ? 1 : 0;
        const uint64_t frequency = (uint64_t)SDL_GetPerformanceFrequency();
        const uint64_t started = (uint64_t)SDL_GetPerformanceCounter();
        uint64_t elapsed_us;
        c_host_raster_resources(surface, &resources);
        target.pixels = (uint16_t *)surface->buffers[target_index];
        target.stride_pixels = surface->stride_bytes / sizeof(uint16_t);
        target.width = surface->width;
        target.height = surface->height;
    target.depth_pixels = surface->raster_depth;
    target.depth_stride_pixels = surface->width;
    target.scratch_mode = surface->raster_scratch_mode;
        memset(&frame_telemetry, 0, sizeof(frame_telemetry));
        status = pxa_raster_validate_draw_list(
            surface->raster_draw_lists[draw_index],
            surface->raster_draw_sizes[draw_index], &target, &resources,
            &list);
        if (status != PXA_STATUS_OK) {
            ++surface->raster_telemetry.rejected_lists;
            surface->raster_draw_pending = -1;
            return 0;
        }
        pxa_raster_execute_draw_list(surface->raster_draw_lists[draw_index],
                                     &list, &target, &resources,
                                     &frame_telemetry);
        elapsed_us = frequency == 0 ? 0 :
            ((uint64_t)SDL_GetPerformanceCounter() - started) *
                UINT64_C(1000000) / frequency;
        surface->raster_telemetry.host_raster_us += elapsed_us;
        surface->raster_telemetry.queue_wait_us +=
            frequency == 0 ? 0
                : started * UINT64_C(1000000) / frequency -
                      surface->raster_draw_queued_us[draw_index];
        surface->raster_telemetry.last_host_raster_us =
            elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;
        surface->raster_telemetry.draw_list_bytes +=
            frame_telemetry.draw_list_bytes;
        surface->raster_telemetry.covered_pixels +=
            frame_telemetry.covered_pixels;
        surface->raster_telemetry.clear_commands +=
            frame_telemetry.clear_commands;
        surface->raster_telemetry.flat_quad_commands +=
            frame_telemetry.flat_quad_commands;
        surface->raster_telemetry.textured_quad_commands +=
            frame_telemetry.textured_quad_commands;
        surface->raster_telemetry.sprite_commands +=
            frame_telemetry.sprite_commands;
        surface->raster_telemetry.last_draw_list_bytes =
            frame_telemetry.last_draw_list_bytes;
        surface->raster_telemetry.last_covered_pixels =
            frame_telemetry.last_covered_pixels;
        surface->pending = (int8_t)target_index;
        surface->pending_frame_id = list.frame_id;
        surface->current = (int8_t)target_index;
        surface->raster_draw_pending = -1;
    }
    if (surface == NULL || (!mapped && !raster) ||
        (mapped && !surface->mapped_registered) ||
        surface->pending < 0 ||
        (mapped && surface->release_count >= surface->buffer_count))
        return 0;
    index = (uint8_t)surface->pending;
    frame_id = surface->pending_frame_id;
    if (!pxa_surface_upscale_rgb565_nearest(
            (const uint16_t *)(mapped ? surface->mapped_buffers[index]
                                     : surface->buffers[index]),
            (uint16_t *)surface->display_buffer, surface->width,
            surface->height, surface->stride_bytes / 2u,
            surface->display_stride_bytes / 2u, surface->scale))
        return 0;
    surface->pending = -1;
    surface->pending_frame_id = 0;
    surface->has_display_frame = 1;
    ++surface->presented_frames;
    if (mapped && !c_host_surface_enqueue_release(surface, index, frame_id))
        return 0;
    status = c_host_surface_present(surface);
    return status == PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_configure(
    void *context, uint64_t provider_surface,
    const pxa_surface_layer_t *layer) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (host == NULL || surface == NULL || surface != host->surface ||
        layer == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    surface->layer = *layer;
    return (surface->current >= 0 || surface->has_display_frame) ?
                                   c_host_surface_present(surface) :
                                   PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_configure_opaque_ui_regions(
    void *context, uint64_t provider_surface,
    const pxa_surface_damage_rect_t *regions, uint8_t region_count) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    if (host == NULL || surface == NULL || surface != host->surface ||
        (region_count != 0 && regions == NULL) ||
        region_count > PXA_SURFACE_MAX_OPAQUE_UI_REGIONS)
        return PXA_STATUS_INVALID_ARGUMENT;
    for (uint8_t index = 0; index < region_count; ++index) {
        const pxa_surface_damage_rect_t *region = &regions[index];
        if (region->width == 0 || region->height == 0 ||
            (uint32_t)region->x + region->width > surface->display_width ||
            (uint32_t)region->y + region->height > surface->display_height) {
            return PXA_STATUS_INVALID_ARGUMENT;
        }
    }
    if (region_count != 0)
        memcpy(surface->opaque_ui_regions, regions,
               (size_t)region_count * sizeof(*regions));
    surface->opaque_ui_region_count = region_count;
    if (c_host_surface_set_fragments(surface, surface->opaque_ui_regions,
                                      surface->opaque_ui_region_count) !=
        PXA_STATUS_OK) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    return (surface->current >= 0 || surface->has_display_frame) ?
                                   c_host_surface_present(surface) :
                                   PXA_STATUS_OK;
}

static pxa_status_t c_host_surface_query(void *context,
                                          uint64_t provider_surface,
                                          pxa_surface_state_t *state) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    uint32_t used;
    if (host == NULL || surface == NULL || surface != host->surface ||
        state == NULL)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->submitted_frames = surface->submitted_frames;
    state->presented_frames = surface->presented_frames;
    state->dropped_frames = surface->dropped_frames;
    state->replaced_frames = surface->replaced_frames;
    state->released_frames = surface->released_frames;
    if ((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) != 0) {
        used = 0;
        for (uint8_t index = 0; index < surface->buffer_count; ++index)
            if (!c_host_surface_mapped_buffer_free(surface, index)) ++used;
    } else {
        used = (surface->current >= 0 ? 1u : 0u) +
               (surface->staged >= 0 ? 1u : 0u);
    }
    state->free_buffers = surface->buffer_count - used;
    state->flags = PXA_SURFACE_STATE_FLAG_SUPPORTS_OPAQUE_UI_REGIONS |
                   PXA_SURFACE_STATE_FLAG_SUPPORTS_ALPHA_COMPOSITING |
                   PXA_SURFACE_STATE_FLAG_SUPPORTS_GUEST_MAPPED;
    {
        pxa_lvgl_ui_alpha_plane_t plane;
        if (pxa_lvgl_ui_alpha_plane(g_host.ui_adapter, &plane))
            state->flags |= PXA_SURFACE_STATE_FLAG_UI_ALPHA_PLANE_ACTIVE;
    }
    return PXA_STATUS_OK;
}

static void c_host_surface_close(void *context, uint64_t provider_surface) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_surface_t *surface =
        (c_host_surface_t *)(uintptr_t)provider_surface;
    uint8_t index;
    if (host == NULL || surface == NULL || surface != host->surface)
        return;
    host->surface = NULL;
    c_host_surface_delete_fragment_objects(surface);
    if (surface->ui_alpha_object != NULL)
        lv_obj_delete(surface->ui_alpha_object);
    free(surface->ui_alpha_bytes);
    free(surface->display_buffer);
    if ((surface->flags & PXA_SURFACE_FLAG_GUEST_MAPPED) == 0) {
        for (index = 0; index < surface->buffer_count; ++index)
            free(surface->buffers[index]);
    }
    for (index = 0; index < PXA_RASTER_MAX_TEXTURES; ++index)
        free(surface->raster_textures[index]);
    for (index = 0; index < 2; ++index)
        free(surface->raster_draw_lists[index]);
    free(surface->raster_depth);
    free(surface->raster_palette);
    free(surface);
}

static pxa_status_t c_host_game_render_create(void *context,
    const pxa_game_render_desc_t *desc, uint64_t *provider, uint32_t *caps) {
    if (!desc || !provider || !caps) return PXA_STATUS_INVALID_ARGUMENT;
    if (desc->scratch_mode > PXA_GAME_RENDER_SCRATCH_COVERAGE_2BIT)
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_surface_desc_t surface_desc = {0};
    surface_desc.width = desc->width; surface_desc.height = desc->height;
    surface_desc.buffer_count = desc->buffer_count;
    surface_desc.format = PXA_SURFACE_FORMAT_RGB565;
    surface_desc.flags = PXA_APP_PAGES_GAME_CONTEXT;
    const uint32_t capacity = desc->max_draw_bytes ? desc->max_draw_bytes : PXA_RASTER_MAX_DRAW_BYTES;
    if (capacity > PXA_RASTER_MAX_DRAW_BYTES) return PXA_STATUS_RESOURCE_LIMIT;
    uint32_t stride;
    pxa_status_t status = c_host_surface_create_impl(context, &surface_desc, provider, &stride, capacity);
    if (status) return status;
    c_host_surface_t *surface = (c_host_surface_t *)(uintptr_t)*provider;
    surface->raster_scratch_mode = desc->scratch_mode;
    if (desc->scratch_mode != PXA_GAME_RENDER_SCRATCH_NONE) {
        const size_t bytes = desc->scratch_mode == PXA_GAME_RENDER_SCRATCH_DEPTH16
            ? (size_t)desc->width * desc->height * sizeof(uint16_t)
            : (((size_t)desc->width + 7u) / 8u) * desc->height * 2u;
        surface->raster_depth = calloc(1, bytes);
        if (!surface->raster_depth) {
            c_host_surface_close(context, *provider); *provider = 0; return PXA_STATUS_RESOURCE_LIMIT;
        }
    }
    *caps = PXA_APP_PAGES_RASTER_CAPS;
    return PXA_STATUS_OK;
}

/* --- audio backend: SDL queue output ------------------------------------ */

static void c_host_audio_release_device(pxa_c_host_t *host) {
    if (host == NULL) return;
    if (host->audio_device != 0) {
        SDL_ClearQueuedAudio(host->audio_device);
        SDL_CloseAudioDevice(host->audio_device);
        host->audio_device = 0;
    }
    if (host->audio_subsystem_owned) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        host->audio_subsystem_owned = 0;
    }
}

static pxa_status_t c_host_audio_ensure_device(pxa_c_host_t *host) {
    SDL_AudioSpec desired;
    if (host == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (host->audio_device != 0) return PXA_STATUS_OK;
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            return PXA_STATUS_UNSUPPORTED;
        }
        host->audio_subsystem_owned = 1;
    }
    SDL_zero(desired);
    desired.freq = PXA_C_HOST_AUDIO_SAMPLE_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = PXA_C_HOST_AUDIO_CHANNELS;
    desired.samples = 512;
    host->audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (host->audio_device == 0) {
        c_host_audio_release_device(host);
        return PXA_STATUS_UNSUPPORTED;
    }
    SDL_PauseAudioDevice(host->audio_device, 0);
    return PXA_STATUS_OK;
}

static c_host_audio_session_t *c_host_audio_find_session(
    pxa_c_host_t *host, uint64_t id) {
    size_t index;
    if (host == NULL || id == 0) return NULL;
    for (index = 0; index < PXA_C_HOST_AUDIO_MAX_SESSIONS; ++index) {
        if (host->audio_sessions[index].active &&
            host->audio_sessions[index].id == id) {
            return &host->audio_sessions[index];
        }
    }
    return NULL;
}

static pxa_status_t c_host_audio_open(void *context, uint16_t usage,
                                      pxa_audio_format_t *format,
                                      uint64_t *provider_session) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    size_t index;
    (void)usage;
    if (host == NULL || format == NULL || provider_session == NULL) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    if (c_host_audio_ensure_device(host) != PXA_STATUS_OK) {
        return PXA_STATUS_UNSUPPORTED;
    }
    for (index = 0; index < PXA_C_HOST_AUDIO_MAX_SESSIONS; ++index) {
        c_host_audio_session_t *slot = &host->audio_sessions[index];
        if (slot->active) continue;
        do {
            ++host->next_audio_session;
        } while (host->next_audio_session == 0 ||
                 c_host_audio_find_session(host,
                                           host->next_audio_session) != NULL);
        memset(slot, 0, sizeof(*slot));
        slot->active = 1;
        slot->id = host->next_audio_session;
        *provider_session = slot->id;
        format->sample_rate = PXA_C_HOST_AUDIO_SAMPLE_RATE;
        format->channels = PXA_C_HOST_AUDIO_CHANNELS;
        format->frame_ms = PXA_C_HOST_AUDIO_FRAME_MS;
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_RESOURCE_LIMIT;
}

static pxa_status_t c_host_audio_commit(void *context,
                                        uint64_t provider_session,
                                        const pxa_audio_graph_t *graph) {
    c_host_audio_session_t *slot = c_host_audio_find_session(
        (pxa_c_host_t *)context, provider_session);
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (graph == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    slot->committed = 1;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_audio_submit(void *context,
                                        uint64_t provider_session,
                                        const uint8_t *pcm, size_t size) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot = c_host_audio_find_session(host,
                                                              provider_session);
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (!slot->committed) return PXA_STATUS_BAD_STATE;
    if (size == 0) return PXA_STATUS_OK;
    if (pcm == NULL || host->audio_device == 0 ||
        size > PXA_C_HOST_AUDIO_MAX_QUEUE_BYTES) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    if ((size_t)SDL_GetQueuedAudioSize(host->audio_device) >
        PXA_C_HOST_AUDIO_MAX_QUEUE_BYTES - size) {
        return PXA_STATUS_WOULD_BLOCK;
    }
    if (SDL_QueueAudio(host->audio_device, pcm, (Uint32)size) != 0) {
        return PXA_STATUS_INTERNAL;
    }
    slot->submitted_samples += size / sizeof(int16_t);
    slot->accepted_samples += size / sizeof(int16_t);
    return PXA_STATUS_OK;
}

static int16_t c_host_audio_tone_sample(uint8_t waveform, uint16_t phase,
                                        uint32_t *noise_state) {
    switch (waveform) {
        case PXA_AUDIO_TONE_SQUARE:
            return (phase & UINT16_C(0x8000)) != 0 ? INT16_MAX : INT16_MIN;
        case PXA_AUDIO_TONE_TRIANGLE: {
            const uint16_t ramp = (phase & UINT16_C(0x8000)) != 0
                                      ? (uint16_t)(UINT16_MAX - phase)
                                      : phase;
            return (int16_t)(((int32_t)ramp - 16384) * 2);
        }
        case PXA_AUDIO_TONE_NOISE:
            *noise_state = *noise_state * UINT32_C(1664525) +
                           UINT32_C(1013904223);
            return (int16_t)(*noise_state >> 16);
        default: {
            const int32_t x = (int16_t)phase;
            const uint32_t magnitude =
                (uint32_t)(x < 0 ? -(int64_t)x : x);
            return (int16_t)((int64_t)4 * x * (32768u - magnitude) / 32768);
        }
    }
}

static pxa_status_t c_host_audio_play_tone(
    void *context, uint64_t provider_session, const pxa_audio_tone_t *tone) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot =
        c_host_audio_find_session(host, provider_session);
    int16_t frame[PXA_C_HOST_AUDIO_SAMPLE_RATE *
                  PXA_C_HOST_AUDIO_FRAME_MS / 1000];
    uint32_t samples;
    uint32_t tone_samples;
    uint32_t delay_samples;
    uint32_t bytes;
    uint32_t phase_step;
    float gain;
    uint32_t noise_state = UINT32_C(0x9e3779b9);
    uint32_t generated = 0;
    uint16_t phase = 0;
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (!slot->committed) return PXA_STATUS_BAD_STATE;
    if (tone == NULL || host->audio_device == 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    tone_samples = (uint32_t)PXA_C_HOST_AUDIO_SAMPLE_RATE *
                   tone->duration_ms / 1000u;
    delay_samples = (uint32_t)PXA_C_HOST_AUDIO_SAMPLE_RATE *
                    tone->delay_ms / 1000u;
    samples = tone_samples + delay_samples;
    bytes = samples * sizeof(frame[0]);
    phase_step = (uint32_t)tone->frequency_hz * UINT32_C(65536) /
                 PXA_C_HOST_AUDIO_SAMPLE_RATE;
    gain = tone->gain_db_q8 == 0
               ? 1.0f
               : powf(10.0f, (float)tone->gain_db_q8 / 5120.0f);
    if (bytes > PXA_C_HOST_AUDIO_MAX_QUEUE_BYTES ||
        (size_t)SDL_GetQueuedAudioSize(host->audio_device) >
            PXA_C_HOST_AUDIO_MAX_QUEUE_BYTES - bytes)
        return PXA_STATUS_WOULD_BLOCK;
    while (generated < samples) {
        const uint32_t count =
            samples - generated > sizeof(frame) / sizeof(frame[0])
                ? sizeof(frame) / sizeof(frame[0])
                : samples - generated;
        uint32_t index;
        for (index = 0; index < count; ++index) {
            const uint32_t position = generated + index;
            uint32_t envelope = 256;
            uint32_t tone_position;
            uint32_t remaining;
            if (position < delay_samples) {
                frame[index] = 0;
                continue;
            }
            tone_position = position - delay_samples;
            remaining = tone_samples - tone_position;
            if (tone->attack_ms != 0) {
                const uint32_t attack_samples =
                    (uint32_t)PXA_C_HOST_AUDIO_SAMPLE_RATE *
                    tone->attack_ms / 1000u;
                if (tone_position < attack_samples)
                    envelope = tone_position * 256u / attack_samples;
            }
            if (tone->release_ms != 0) {
                const uint32_t release_samples =
                    (uint32_t)PXA_C_HOST_AUDIO_SAMPLE_RATE *
                    tone->release_ms / 1000u;
                if (remaining <= release_samples) {
                    const uint32_t release_envelope =
                        remaining * 256u / release_samples;
                    if (release_envelope < envelope)
                        envelope = release_envelope;
                }
            }
            const int32_t raw = c_host_audio_tone_sample(
                tone->waveform, phase, &noise_state);
            frame[index] = (int16_t)(raw * gain * envelope / 256.0f);
            phase = (uint16_t)(phase + phase_step);
        }
        if (SDL_QueueAudio(host->audio_device, frame,
                           (Uint32)(count * sizeof(frame[0]))) != 0)
            return PXA_STATUS_INTERNAL;
        generated += count;
    }
    slot->submitted_samples += samples;
    slot->accepted_samples += samples;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_audio_play_asset(
    void *context, uint64_t provider_session,
    const pxa_audio_asset_t *asset) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot = c_host_audio_find_session(
        host, provider_session);
    const pxa_package_file_t *file;
    int path_size;
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (!slot->committed) return PXA_STATUS_BAD_STATE;
    if (asset == NULL || asset->path == NULL || asset->path_size < 5 ||
        host->active_manifest == NULL || host->active_app == NULL)
        return PXA_STATUS_NOT_FOUND;
    file = pxa_package_file_find(
        host->active_manifest,
        (pxa_bytes_t){asset->path, asset->path_size});
    if (file == NULL ||
        memcmp(asset->path + asset->path_size - 4u, ".ogg", 4) != 0)
        return PXA_STATUS_NOT_FOUND;
    path_size = snprintf(slot->asset_path, sizeof(slot->asset_path),
                         "%s/%.*s", host->active_app->root,
                         (int)asset->path_size, (const char *)asset->path);
    if (path_size <= 0 || path_size >= (int)sizeof(slot->asset_path))
        return PXA_STATUS_RESOURCE_LIMIT;
    slot->asset_gain_db_q8 = asset->gain_db_q8;
    slot->asset_loop =
        (asset->flags & PXA_AUDIO_ASSET_LOOP) != 0;
    slot->asset_paused = 0;
    slot->asset_active = 1;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_audio_control_asset(
    void *context, uint64_t provider_session,
    const pxa_audio_asset_control_t *control) {
    c_host_audio_session_t *slot = c_host_audio_find_session(
        (pxa_c_host_t *)context, provider_session);
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (control == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    if (!slot->asset_active) return PXA_STATUS_BAD_STATE;
    switch (control->action) {
        case PXA_AUDIO_ASSET_PAUSE:
            slot->asset_paused = 1;
            break;
        case PXA_AUDIO_ASSET_RESUME:
            slot->asset_paused = 0;
            break;
        case PXA_AUDIO_ASSET_STOP:
            slot->asset_active = 0;
            slot->asset_paused = 0;
            slot->asset_path[0] = '\0';
            break;
        case PXA_AUDIO_ASSET_SET_GAIN:
            slot->asset_gain_db_q8 = control->gain_db_q8;
            break;
        default:
            return PXA_STATUS_INVALID_ARGUMENT;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_audio_query(void *context,
                                       uint64_t provider_session,
                                       pxa_audio_state_t *state) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot =
        c_host_audio_find_session(host, provider_session);
    uint64_t queued_samples;
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (state == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    queued_samples = host->audio_device == 0
                         ? 0
                         : SDL_GetQueuedAudioSize(host->audio_device) /
                               sizeof(int16_t);
    if (queued_samples > slot->submitted_samples)
        queued_samples = slot->submitted_samples;
    state->submitted_samples = slot->submitted_samples;
    state->accepted_samples = slot->accepted_samples;
    state->queued_samples = (uint32_t)queued_samples;
    state->flags = PXA_AUDIO_STATE_ACCEPTED_IS_SINK_SUBMITTED;
    return PXA_STATUS_OK;
}

static pxa_status_t c_host_audio_flush(void *context,
                                       uint64_t provider_session) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot =
        c_host_audio_find_session(host, provider_session);
    if (slot == NULL) return PXA_STATUS_NOT_FOUND;
    if (host->audio_device == 0) return PXA_STATUS_BAD_STATE;
    SDL_ClearQueuedAudio(host->audio_device);
    return PXA_STATUS_OK;
}

static void c_host_audio_close(void *context, uint64_t provider_session) {
    pxa_c_host_t *host = (pxa_c_host_t *)context;
    c_host_audio_session_t *slot = c_host_audio_find_session(host,
                                                              provider_session);
    size_t index;
    if (slot == NULL) return;
    memset(slot, 0, sizeof(*slot));
    for (index = 0; index < PXA_C_HOST_AUDIO_MAX_SESSIONS; ++index) {
        if (host->audio_sessions[index].active) return;
    }
    if (host->audio_device != 0) {
        SDL_ClearQueuedAudio(host->audio_device);
    }
}

/* --- canvas async executor: synchronous (single LVGL thread) ------------ */

static pxa_status_t c_host_sync_execute(
    pxa_lvgl_ui_execute_callback_fn callback, void *callback_data,
    void *user_data) {
    (void)user_data;
    callback(callback_data);
    return PXA_STATUS_OK;
}

/* --- host Clock service: set period, host-driven ticks ------------------ */

static pxa_status_t c_host_clock_control(void *context, pxa_runtime_t *runtime,
                                         pxa_component_t component,
                                         const pxa_message_view_t *message) {
    uint8_t response[PXA_ENVELOPE_SIZE + 12];
    uint8_t payload[12];
    pxa_writer_t writer;
    uint64_t frequency;
    uint64_t timestamp_us;
    (void)context;
    if (message->opcode == PXA_CLOCK_WALL_TIME)
        return pxa_posix_clock_control(runtime, component, message, host_now_us(NULL));
    if (message->opcode == PXA_CLOCK_NOW) {
        if (message->request_token == 0 || message->payload.size != 0) {
            return PXA_STATUS_INVALID_ARGUMENT;
        }
        frequency = (uint64_t)SDL_GetPerformanceFrequency();
        timestamp_us = frequency == 0 ? 0 :
            (uint64_t)SDL_GetPerformanceCounter() * UINT64_C(1000000) /
            frequency;
        uint16_t profile;
        pxa_status_t status = pxa_component_wire_profile(runtime, component, &profile);
        if (status != PXA_STATUS_OK) return status;
        if (profile == 1) {
            status = pxa_request_begin_reserved(runtime, component,
                message->request_token, PXA_CLOCK_SERVICE_ID, PXA_CLOCK_NOW,
                0, sizeof(timestamp_us));
            if (status != PXA_STATUS_OK) return status;
            status = pxa_request_commit(runtime, component, message->request_token);
            if (status != PXA_STATUS_OK) return status;
            pxa_write_u64(payload, timestamp_us);
            return pxa_request_complete(runtime, component, message->request_token,
                PXA_STATUS_OK, payload, sizeof(timestamp_us));
        }
        pxa_write_u32(payload, PXA_STATUS_OK);
        pxa_write_u64(payload + 4, timestamp_us);
        pxa_writer_init(&writer, response, sizeof(response));
        if (pxa_writer_message(&writer, PXA_CLOCK_SERVICE_ID,
                               PXA_CLOCK_NOW_RESULT, message->request_token,
                               payload, sizeof(payload)) != PXA_STATUS_OK) {
            return PXA_STATUS_INTERNAL;
        }
        return pxa_event_post(
            runtime, component, response, writer.size, 0,
            ((uint64_t)PXA_CLOCK_SERVICE_ID << 48) |
                ((uint64_t)PXA_CLOCK_NOW_RESULT << 32) |
                message->request_token);
    }
    if (message->opcode != PXA_CLOCK_SET_PERIOD || message->request_token != 0 ||
        message->payload.size != 2) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    {
        const uint16_t period_ms = pxa_read_u16(message->payload.data);
        if (period_ms != 0 && (period_ms < 16 || period_ms > 1000))
            return PXA_STATUS_INVALID_ARGUMENT;
        g_host.clock_period_ms = period_ms;
        g_host.clock_last_tick_us = g_host.fake_now_us;
    }
    (void)component;
    return PXA_STATUS_OK;
}

/* --- engine prepare_start: bind window backend --------------------------- */

static pxa_status_t c_host_set_ui_startup_config(
    uint64_t instance_id, pxa_component_t component) {
    uint8_t config[PXA_WAMR_ENGINE_MAX_CONFIG_BYTES];
    uint8_t payload[8];
    uint8_t environment_bytes[128];
    uint8_t system_environment[4u + PXA_SYSTEM_LOCALE_MAX_BYTES + 5u];
    size_t environment_size;
    pxa_ui_environment_t environment;
    pxa_writer_t writer;
    pxa_writer_t system_writer;
    pxa_status_t status = pxa_ui_get_environment(
        g_host.ui_service, component, PXA_UI_PRIMARY_SURFACE, &environment);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_ui_encode_environment(
        &environment, environment_bytes, sizeof(environment_bytes),
        &environment_size);
    if (status != PXA_STATUS_OK) return status;
    pxa_writer_init(&writer, config, sizeof(config));
    pxa_write_u16(payload, PXA_UI_SERVICE_ID);
    pxa_write_u16(payload + 2, PXA_UI_SERVICE_MAJOR);
    pxa_write_u16(payload + 4, PXA_UI_SERVICE_MINOR);
    pxa_write_u16(payload + 6, 0);
    if (pxa_writer_record(&writer, PXA_CONFIG_SERVICE_VERSION, payload, 8) !=
        PXA_STATUS_OK) return writer.status;
    if (pxa_writer_record(&writer, PXA_UI_CONFIG_ENVIRONMENT,
                          environment_bytes, environment_size) !=
        PXA_STATUS_OK) return writer.status;
    pxa_writer_init(&system_writer, system_environment,
                    sizeof(system_environment));
    if (pxa_writer_record(&system_writer, PXA_SYSTEM_CONFIGURATION_LOCALE,
                          g_host.locale, g_host.locale_size) != PXA_STATUS_OK ||
        pxa_writer_record(&system_writer,
                          PXA_SYSTEM_CONFIGURATION_TEXT_DIRECTION,
                          &g_host.text_direction, 1u) != PXA_STATUS_OK ||
        pxa_writer_record(&writer, PXA_SYSTEM_CONFIG_ENVIRONMENT,
                          system_writer.data, system_writer.size) !=
            PXA_STATUS_OK) {
        return writer.status != PXA_STATUS_OK ? writer.status
                                               : system_writer.status;
    }
    return pxa_wamr_engine_set_config(
        g_host.engine, instance_id,
        (pxa_bytes_t){writer.data, writer.size});
}

static pxa_status_t c_host_prepare_start(void *context,
                                         pxa_component_t component,
                                         uint64_t instance_id, uint8_t kind) {
    pxa_window_backend_t backend;
    pxa_status_t status;
    (void)context;
    if (kind != PXA_COMPONENT_KIND_UI) return PXA_STATUS_OK;
    memset(&backend, 0, sizeof(backend));
    backend.struct_size = sizeof(backend);
    backend.apply = c_host_window_apply;
    status = pxa_window_bind(g_host.window_service, component, &backend);
    if (status != PXA_STATUS_OK) return status;
    {
        pxa_window_snapshot_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.logical_width = g_display_metrics.width;
        snapshot.logical_height = g_display_metrics.height;
        snapshot.pixel_width = g_display_metrics.width;
        snapshot.pixel_height = g_display_metrics.height;
        snapshot.safe_insets.left = g_display_metrics.safe_insets[3];
        snapshot.safe_insets.top = g_display_metrics.safe_insets[0];
        snapshot.safe_insets.right = g_display_metrics.safe_insets[1];
        snapshot.safe_insets.bottom = g_display_metrics.safe_insets[2];
        snapshot.density_numerator = 1;
        snapshot.density_denominator = 1;
        snapshot.focused = 1;
        (void)pxa_window_update_snapshot(g_host.window_service, component,
                                         &snapshot);
    }
    status = pxa_ui_bind(g_host.ui_service, component, &g_host.ui_backend);
    if (status != PXA_STATUS_OK) {
        (void)pxa_window_unbind(g_host.window_service, component);
        return status;
    }
    status = c_host_set_ui_startup_config(instance_id, component);
    if (status != PXA_STATUS_OK) {
        (void)pxa_ui_unbind(g_host.ui_service, component);
        (void)pxa_window_unbind(g_host.window_service, component);
        return status;
    }
    g_host.ui_component = component;
    return PXA_STATUS_OK;
}

/* --- package catalog: scan builtins and install them --------------------- */

static int c_host_add_permission_declaration(pxa_bytes_t name,
                                             pxa_bytes_t scope,
                                             uint16_t *index_out) {
    uint16_t index;
    pxa_permission_declaration_t *target;
    char *name_storage;
    char *scope_storage;
    if (name.data == NULL || name.size == 0 ||
        name.size >= PXA_C_HOST_PERMISSION_NAME_MAX ||
        scope.size >= PXA_C_HOST_PERMISSION_SCOPE_MAX) {
        return 0;
    }
    for (index = 0; index < g_host.declaration_count; ++index) {
        const pxa_permission_declaration_t *existing =
            &g_host.declarations[index];
        if (existing->name.size == name.size &&
            existing->scope.size == scope.size &&
            memcmp(existing->name.data, name.data, name.size) == 0 &&
            (scope.size == 0 ||
             memcmp(existing->scope.data, scope.data, scope.size) == 0)) {
            if (index_out != NULL) *index_out = index;
            return 1;
        }
    }
    if (g_host.declaration_count >= PXA_C_HOST_MAX_DECLARATIONS) return 0;
    target = &g_host.declarations[g_host.declaration_count];
    name_storage = g_host.declaration_name[g_host.declaration_count];
    scope_storage = g_host.declaration_scope[g_host.declaration_count];
    memcpy(name_storage, name.data, name.size);
    name_storage[name.size] = '\0';
    if (scope.size != 0) memcpy(scope_storage, scope.data, scope.size);
    scope_storage[scope.size] = '\0';
    target->name = (pxa_bytes_t){(const uint8_t *)name_storage, name.size};
    target->scope = (pxa_bytes_t){(const uint8_t *)scope_storage, scope.size};
    /* Required status is package-specific and is checked before activation. */
    target->required = 0;
    if (index_out != NULL) *index_out = g_host.declaration_count;
    ++g_host.declaration_count;
    return 1;
}

static int c_host_add_app_permission(pxa_c_host_app_t *app, pxa_bytes_t name,
                                     pxa_bytes_t scope, uint8_t required) {
    uint16_t index;
    uint16_t declaration_index;
    c_host_app_permission_t *target;
    if (app == NULL || name.data == NULL || name.size == 0 ||
        name.size >= PXA_C_HOST_PERMISSION_NAME_MAX ||
        scope.size >= PXA_C_HOST_PERMISSION_SCOPE_MAX || required > 1) {
        return 0;
    }
    if (!c_host_add_permission_declaration(name, scope, &declaration_index)) {
        return 0;
    }
    for (index = 0; index < app->permission_count; ++index) {
        c_host_app_permission_t *existing = &app->permissions[index];
        if (existing->declaration_index == declaration_index) {
            existing->required = required != 0;
            return 1;
        }
    }
    if (app->permission_count >= PXA_C_HOST_MAX_APP_PERMISSIONS) return 0;
    target = &app->permissions[app->permission_count];
    target->declaration_index = declaration_index;
    target->required = required != 0;
    target->granted = 0;
    ++app->permission_count;
    return 1;
}

static int c_host_add_job_component(
    pxa_bytes_t component_id, pxa_bytes_t components[PXA_C_HOST_MAX_JOBS],
    char names[PXA_C_HOST_MAX_JOBS][65], uint16_t *count) {
    uint16_t index;
    if (component_id.data == NULL || component_id.size == 0 ||
        component_id.size > 64 || count == NULL) {
        return 0;
    }
    for (index = 0; index < *count; ++index) {
        if (components[index].size == component_id.size &&
            memcmp(components[index].data, component_id.data, component_id.size) == 0) {
            return 1;
        }
    }
    if (*count >= PXA_C_HOST_MAX_JOBS)
        return 0;
    memcpy(names[*count], component_id.data, component_id.size);
    names[*count][component_id.size] = '\0';
    components[*count] = (pxa_bytes_t){(const uint8_t*)names[*count], component_id.size};
    ++*count;
    return 1;
}

static int c_host_collect_manifest_declarations(pxa_c_host_app_t* app,
                                                const pxa_package_manifest_t* manifest) {
    pxa_c_host_app_t candidate;
    uint16_t declaration_checkpoint;
    uint16_t component_index;
    uint16_t permission_index;
    size_t job_count = 0;
    if (app == NULL || manifest == NULL ||
        manifest->permission_count > PXA_C_HOST_MAX_APP_PERMISSIONS) {
        return 0;
    }
    for (component_index = 0; component_index < manifest->component_count; ++component_index) {
        if (manifest->components[component_index].kind == 3)
            ++job_count;
    }
    if (job_count > PXA_C_HOST_MAX_JOBS)
        return 0;
    candidate = *app;
    memset(candidate.permissions, 0, sizeof(candidate.permissions));
    memset(candidate.job_components, 0, sizeof(candidate.job_components));
    memset(candidate.job_component_names, 0, sizeof(candidate.job_component_names));
    candidate.permission_count = 0;
    candidate.job_component_count = 0;
    declaration_checkpoint = g_host.declaration_count;
    for (component_index = 0; component_index < manifest->component_count; ++component_index) {
        const pxa_package_component_t* component = &manifest->components[component_index];
        if (component->kind == 3 &&
            !c_host_add_job_component(component->id, candidate.job_components,
                                      candidate.job_component_names,
                                      &candidate.job_component_count)) {
            goto rollback;
        }
    }
    for (permission_index = 0; permission_index < manifest->permission_count; ++permission_index) {
        const pxa_package_permission_t* permission = &manifest->permissions[permission_index];
        if (!c_host_add_app_permission(&candidate, permission->name, permission->scope,
                                       permission->required)) {
            goto rollback;
        }
    }
    *app = candidate;
    c_host_rebase_app_views(app);
    return 1;

rollback:
    if (declaration_checkpoint < g_host.declaration_count) {
        const size_t added = (size_t)(g_host.declaration_count - declaration_checkpoint);
        memset(&g_host.declarations[declaration_checkpoint], 0,
               added * sizeof(g_host.declarations[0]));
        memset(&g_host.declaration_name[declaration_checkpoint], 0,
               added * sizeof(g_host.declaration_name[0]));
        memset(&g_host.declaration_scope[declaration_checkpoint], 0,
               added * sizeof(g_host.declaration_scope[0]));
        g_host.declaration_count = declaration_checkpoint;
    }
    return 0;
}

static void c_host_rebase_app_views(pxa_c_host_app_t* app) {
    uint16_t index;
    if (app == NULL)
        return;
    for (index = 0; index < app->job_component_count; ++index) {
        app->job_components[index].data = (const uint8_t*)app->job_component_names[index];
    }
}

static void c_host_compact_permission_declarations(void) {
    uint8_t used[PXA_C_HOST_MAX_DECLARATIONS];
    uint16_t remap[PXA_C_HOST_MAX_DECLARATIONS];
    uint16_t old_index;
    uint16_t new_count = 0;
    size_t app_index;
    memset(used, 0, sizeof(used));
    memset(remap, 0xff, sizeof(remap));
    for (app_index = 0; app_index < g_host.app_count; ++app_index) {
        pxa_c_host_app_t* app = &g_host.apps[app_index];
        uint16_t permission_index;
        for (permission_index = 0; permission_index < app->permission_count; ++permission_index) {
            const uint16_t declaration_index = app->permissions[permission_index].declaration_index;
            if (declaration_index < g_host.declaration_count) {
                used[declaration_index] = 1;
            }
        }
    }
    for (old_index = 0; old_index < g_host.declaration_count; ++old_index) {
        pxa_permission_declaration_t* source;
        pxa_permission_declaration_t* target;
        if (!used[old_index])
            continue;
        source = &g_host.declarations[old_index];
        target = &g_host.declarations[new_count];
        if (new_count != old_index) {
            memmove(g_host.declaration_name[new_count], source->name.data, source->name.size);
            memmove(g_host.declaration_scope[new_count], source->scope.data, source->scope.size);
        }
        g_host.declaration_name[new_count][source->name.size] = '\0';
        g_host.declaration_scope[new_count][source->scope.size] = '\0';
        target->name =
            (pxa_bytes_t){(const uint8_t*)g_host.declaration_name[new_count], source->name.size};
        target->scope =
            (pxa_bytes_t){(const uint8_t*)g_host.declaration_scope[new_count], source->scope.size};
        target->required = 0;
        remap[old_index] = new_count++;
    }
    for (app_index = 0; app_index < g_host.app_count; ++app_index) {
        pxa_c_host_app_t* app = &g_host.apps[app_index];
        uint16_t permission_index;
        for (permission_index = 0; permission_index < app->permission_count; ++permission_index) {
            const uint16_t old_declaration = app->permissions[permission_index].declaration_index;
            if (old_declaration < g_host.declaration_count &&
                remap[old_declaration] != UINT16_MAX) {
                app->permissions[permission_index].declaration_index = remap[old_declaration];
            }
        }
    }
    if (new_count < g_host.declaration_count) {
        memset(&g_host.declarations[new_count], 0,
               (g_host.declaration_count - new_count) *
                   sizeof(g_host.declarations[0]));
    }
    g_host.declaration_count = new_count;
}

static void c_host_scan_builtins(void) {
    DIR *dir;
    struct dirent *entry;
    c_host_manifest_buffers_t buffers = {0};
    dir = opendir(g_host.builtins_root);
    if (dir == NULL) return;
    while ((entry = readdir(dir)) != NULL) {
        char path[PXA_C_HOST_MAX_PATH];
        char app_id[PXA_C_HOST_MAX_APP_ID];
        pxa_package_manifest_t *manifest = NULL;
        pxa_c_host_app_t *app;
        int path_size;
        if (entry->d_name[0] == '.') continue;
        if (g_host.app_count >= PXA_C_HOST_MAX_APPS) break;
        path_size = snprintf(path, sizeof(path), "%s/%s",
                             g_host.builtins_root, entry->d_name);
        if (path_size <= 0 || (size_t)path_size >= sizeof(path) ||
            !c_host_read_manifest(path, &buffers, &manifest) ||
            manifest->app_id.size == 0 ||
            manifest->app_id.size >= sizeof(app_id)) {
            continue;
        }
        memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
        app_id[manifest->app_id.size] = '\0';
        if (c_host_find_app(app_id) != NULL) continue;
        app = &g_host.apps[g_host.app_count++];
        memset(app, 0, sizeof(*app));
        snprintf(app->id, sizeof(app->id), "%s", app_id);
        c_host_copy_manifest_metadata(app, manifest);
        if (app->name[0] == '\0') {
            snprintf(app->name, sizeof(app->name), "%s", entry->d_name);
        }
        snprintf(app->source, sizeof(app->source), "%s", path);
        snprintf(app->root, sizeof(app->root), "%s", path);
        app->enabled = 1;
        app->built_in = 1;
    }

done:
    c_host_manifest_buffers_release(&buffers);
    closedir(dir);
}

static int c_host_manifest_matches(const pxa_package_manifest_t *manifest,
                                   const char *app_id) {
    size_t id_size = app_id == NULL ? 0 : strlen(app_id);
    return manifest != NULL && manifest->app_id.data != NULL &&
           manifest->app_id.size == id_size &&
           memcmp(manifest->app_id.data, app_id, id_size) == 0;
}

static void c_host_copy_manifest_metadata(
    pxa_c_host_app_t *app, const pxa_package_manifest_t *manifest) {
    if (manifest->name.size != 0 && manifest->name.size < sizeof(app->name)) {
        memcpy(app->name, manifest->name.data, manifest->name.size);
        app->name[manifest->name.size] = '\0';
    }
    if (manifest->version.size < sizeof(app->version)) {
        memcpy(app->version, manifest->version.data, manifest->version.size);
        app->version[manifest->version.size] = '\0';
    }
    if (manifest->icon_path.size < sizeof(app->icon_path)) {
        memcpy(app->icon_path, manifest->icon_path.data,
               manifest->icon_path.size);
        app->icon_path[manifest->icon_path.size] = '\0';
    }
}

static int c_host_apply_installed_manifest(
    pxa_c_host_app_t *app, const pxa_package_manifest_t *manifest,
    const char *installed_root) {
    if (!c_host_collect_manifest_declarations(app, manifest)) return 0;
    snprintf(app->root, sizeof(app->root), "%s", installed_root);
    memcpy(app->publisher_key_id, manifest->management_key_id,
           PXA_PACKAGE_DIGEST_BYTES);
    app->has_publisher_key_id = 1;
    app->installed = 1;
    c_host_copy_manifest_metadata(app, manifest);
    c_host_load_app_permission_policy(app);
    return 1;
}

static int c_host_install_app(pxa_c_host_app_t *app) {
    pxa_posix_installer_result_t result;
    pxa_posix_install_disposition_t disposition;
    pxa_package_manifest_t *manifest = NULL;
    c_host_manifest_buffers_t buffers = {0};
    char installed_root[PXA_C_HOST_MAX_PATH];
    pxa_status_t status = PXA_STATUS_RESOURCE_LIMIT;
    if (app == NULL || app->source[0] == '\0') return 0;
    if (!c_host_verify_package_source(app->source, &buffers, &manifest))
        goto done;
    manifest = NULL;
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest_workspace = buffers.workspace;
    result.manifest_workspace_size = buffers.workspace_size;
    result.encoded = buffers.encoded;
    result.encoded_capacity = buffers.encoded_capacity;
    result.manifest = &manifest;
    result.root = installed_root;
    result.root_capacity = sizeof(installed_root);
    status = pxa_posix_installer_install(g_host.installer, app->source,
                                         &result, &disposition);
    if (status == PXA_STATUS_OK && c_host_manifest_matches(manifest, app->id) &&
        c_host_apply_installed_manifest(app, manifest, installed_root)) {
    } else {
        status = status == PXA_STATUS_OK ? PXA_STATUS_DENIED : status;
    }
done:
    c_host_manifest_buffers_release(&buffers);
    return status == PXA_STATUS_OK;
}

static int c_host_read_manifest(const char *root,
                                c_host_manifest_buffers_t *buffers,
                                pxa_package_manifest_t **manifest) {
    char path[PXA_C_HOST_MAX_PATH];
    struct stat metadata;
    size_t offset = 0;
    int path_size;
    int file;
    path_size = snprintf(path, sizeof(path), "%s/manifest.pxm", root);
    if (buffers == NULL || manifest == NULL || path_size <= 0 ||
        (size_t)path_size >= sizeof(path)) return 0;
    file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0 || fstat(file, &metadata) != 0 || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size > SIZE_MAX ||
        !c_host_manifest_buffers_ensure_encoded(
            buffers, (size_t)metadata.st_size)) {
        if (file >= 0) close(file);
        return 0;
    }
    while (offset < (size_t)metadata.st_size) {
        ssize_t count = read(file, buffers->encoded + offset,
                             (size_t)metadata.st_size - offset);
        if (count <= 0) {
            close(file);
            return 0;
        }
        offset += (size_t)count;
    }
    close(file);
    if (!c_host_manifest_buffers_prepare_workspace(buffers, offset)) return 0;
    *manifest = NULL;
    return buffers->has_parsed_limits &&
           pxa_package_manifest_parse(
               buffers->workspace, buffers->workspace_size,
               (pxa_bytes_t){buffers->encoded, offset},
               &buffers->parsed_limits, manifest) == PXA_STATUS_OK;
}

static int c_host_verify_package_source(
    const char *source, c_host_manifest_buffers_t *buffers,
    pxa_package_manifest_t **manifest) {
    pxa_posix_installer_result_t result;
    char verified_root[PXA_C_HOST_MAX_PATH];
    size_t manifest_size;
    pxa_status_t status;
    if (source == NULL || buffers == NULL || manifest == NULL ||
        pxa_posix_installer_source_manifest_size(g_host.installer, source,
                                                 &manifest_size) !=
            PXA_STATUS_OK ||
        !c_host_manifest_buffers_ensure_encoded(buffers, manifest_size) ||
        !c_host_manifest_buffers_ensure_workspace(buffers, 1)) {
        return 0;
    }
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest_workspace = buffers->workspace;
    result.manifest_workspace_size = buffers->workspace_size;
    result.encoded = buffers->encoded;
    result.encoded_capacity = buffers->encoded_capacity;
    result.manifest = manifest;
    result.root = verified_root;
    result.root_capacity = sizeof(verified_root);
    *manifest = NULL;
    status = pxa_posix_installer_verify_source(g_host.installer, source,
                                               &result);
    if (status != PXA_STATUS_RESOURCE_LIMIT ||
        !c_host_manifest_buffers_prepare_workspace(buffers, manifest_size)) {
        return status == PXA_STATUS_OK;
    }
    result.manifest_workspace = buffers->workspace;
    result.manifest_workspace_size = buffers->workspace_size;
    *manifest = NULL;
    return pxa_posix_installer_verify_source(g_host.installer, source,
                                             &result) == PXA_STATUS_OK;
}

static int c_host_load_current_app(pxa_c_host_app_t *app,
                                   c_host_manifest_buffers_t *buffers,
                                   pxa_package_manifest_t **manifest,
                                   char *installed_root,
                                   size_t installed_root_capacity) {
    pxa_posix_installer_identity_t identity;
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *preflight = NULL;
    if (app == NULL || buffers == NULL || manifest == NULL ||
        !app->has_publisher_key_id ||
        !c_host_read_manifest(app->root, buffers, &preflight)) {
        return 0;
    }
    memset(&identity, 0, sizeof(identity));
    identity.app_id =
        (pxa_bytes_t){(const uint8_t *)app->id, strlen(app->id)};
    identity.publisher_key_id =
        (pxa_bytes_t){app->publisher_key_id, PXA_PACKAGE_DIGEST_BYTES};
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest_workspace = buffers->workspace;
    result.manifest_workspace_size = buffers->workspace_size;
    result.encoded = buffers->encoded;
    result.encoded_capacity = buffers->encoded_capacity;
    result.manifest = manifest;
    result.root = installed_root;
    result.root_capacity = installed_root_capacity;
    return pxa_posix_installer_load_current(g_host.installer, &identity,
                                            &result) == PXA_STATUS_OK &&
           c_host_manifest_matches(*manifest, app->id);
}

static void c_host_scan_installed(void) {
    char packages_root[PXA_C_HOST_MAX_PATH];
    DIR *directory;
    struct dirent *entry;
    c_host_manifest_buffers_t buffers = {0};
    int root_size = snprintf(packages_root, sizeof(packages_root), "%s/packages",
                             g_host.storage_root);
    if (root_size <= 0 || (size_t)root_size >= sizeof(packages_root)) return;
    directory = opendir(packages_root);
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char root[PXA_C_HOST_MAX_PATH];
        char installed_root[PXA_C_HOST_MAX_PATH];
        pxa_package_manifest_t *manifest = NULL;
        pxa_c_host_app_t *app;
        int path_size;
        int created = 0;
        if (entry->d_name[0] == '.') continue;
        path_size = snprintf(root, sizeof(root), "%s/%s", packages_root,
                             entry->d_name);
        if (path_size <= 0 || (size_t)path_size >= sizeof(root) ||
            !c_host_read_manifest(root, &buffers, &manifest) ||
            manifest->app_id.size >= PXA_C_HOST_MAX_APP_ID ||
            strlen(entry->d_name) != manifest->app_id.size ||
            memcmp(entry->d_name, manifest->app_id.data, manifest->app_id.size) != 0) {
            continue;
        }
        app = c_host_find_app(entry->d_name);
        if (app != NULL && app->installed)
            continue;
        if (app == NULL) {
            if (g_host.app_count >= PXA_C_HOST_MAX_APPS)
                break;
            app = &g_host.apps[g_host.app_count++];
            memset(app, 0, sizeof(*app));
            snprintf(app->id, sizeof(app->id), "%s", entry->d_name);
            app->enabled = 1;
            created = 1;
        }
        snprintf(app->root, sizeof(app->root), "%s", root);
        memcpy(app->publisher_key_id, manifest->management_key_id,
               PXA_PACKAGE_DIGEST_BYTES);
        app->has_publisher_key_id = 1;
        manifest = NULL;
        if (!c_host_load_current_app(app, &buffers, &manifest, installed_root,
                                     sizeof(installed_root))) {
            if (created) {
                memset(app, 0, sizeof(*app));
                --g_host.app_count;
            }
            continue;
        }
        if (!c_host_apply_installed_manifest(app, manifest, installed_root)) {
            if (created) {
                memset(app, 0, sizeof(*app));
                --g_host.app_count;
            }
        }
    }

done:
    c_host_manifest_buffers_release(&buffers);
    closedir(directory);
    c_host_apply_disabled_policy();
}

static void c_host_remove_stale_apps(void) {
    size_t index = 0;
    int removed = 0;
    if (g_host.active_app != NULL)
        return;
    while (index < g_host.app_count) {
        const pxa_c_host_app_t* app = &g_host.apps[index];
        if (!app->built_in && !app->installed && !app->staged) {
            memmove(&g_host.apps[index], &g_host.apps[index + 1u],
                    (g_host.app_count - index - 1u) * sizeof(g_host.apps[0]));
            --g_host.app_count;
            memset(&g_host.apps[g_host.app_count], 0, sizeof(g_host.apps[0]));
            removed = 1;
            continue;
        }
        ++index;
    }
    if (removed) {
        for (index = 0; index < g_host.app_count; ++index) {
            c_host_rebase_app_views(&g_host.apps[index]);
        }
        c_host_compact_permission_declarations();
    }
}

static void c_host_scan_inbox(void) {
    DIR *directory;
    struct dirent *entry;
    c_host_manifest_buffers_t buffers = {0};
    size_t index;
    int scanned = 0;
    directory = opendir(g_host.inbox_root);
    if (directory == NULL) return;
    for (index = 0; index < g_host.app_count; ++index) {
        if (g_host.apps[index].built_in) continue;
        g_host.apps[index].staged = 0;
        if (!g_host.apps[index].installed) {
            g_host.apps[index].source[0] = '\0';
        }
    }
    while ((entry = readdir(directory)) != NULL) {
        char source[PXA_C_HOST_MAX_PATH];
        char app_id[PXA_C_HOST_MAX_APP_ID];
        pxa_package_manifest_t *manifest = NULL;
        pxa_c_host_app_t *app;
        int source_size;
        if (entry->d_name[0] == '.') continue;
        source_size = snprintf(source, sizeof(source), "%s/%s",
                               g_host.inbox_root, entry->d_name);
        if (source_size <= 0 || (size_t)source_size >= sizeof(source) ||
            !c_host_verify_package_source(source, &buffers,
                                          &manifest) ||
            manifest->app_id.size == 0 ||
            manifest->app_id.size >= sizeof(app_id)) {
            continue;
        }
        memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
        app_id[manifest->app_id.size] = '\0';
        app = c_host_find_app(app_id);
        if (app != NULL && app->built_in) continue;
        if (app != NULL && app->staged) continue;
        if (app == NULL) {
            if (g_host.app_count >= PXA_C_HOST_MAX_APPS) continue;
            app = &g_host.apps[g_host.app_count++];
            memset(app, 0, sizeof(*app));
            snprintf(app->id, sizeof(app->id), "%s", app_id);
            app->enabled = 1;
        }
        snprintf(app->source, sizeof(app->source), "%s", source);
        app->staged = 1;
        if (!app->installed) {
            snprintf(app->root, sizeof(app->root), "%s", source);
            c_host_copy_manifest_metadata(app, manifest);
        }
    }
    scanned = 1;

done:
    c_host_manifest_buffers_release(&buffers);
    closedir(directory);
    if (scanned) {
        c_host_remove_stale_apps();
        c_host_apply_disabled_policy();
    }
}

static int c_host_path_is_within(const char *path, const char *directory) {
    const size_t size = directory == NULL ? 0 : strlen(directory);
    return path != NULL && size != 0 && strncmp(path, directory, size) == 0 &&
           (path[size] == '\0' || path[size] == '/');
}

static int c_host_copy_manifest_text(char *output, size_t output_capacity,
                                     pxa_bytes_t value) {
    if (output == NULL || output_capacity == 0 || value.data == NULL ||
        value.size >= output_capacity) {
        return 0;
    }
    memcpy(output, value.data, value.size);
    output[value.size] = '\0';
    return 1;
}

static bool c_host_preview_package_cb(const char *source_path,
                                      app_pages_package_preview_t *preview,
                                      void *user_data) {
    c_host_manifest_buffers_t buffers = {0};
    pxa_package_manifest_t *manifest = NULL;
    pxa_c_host_app_t *installed;
    struct stat metadata;
    const char *name;
    size_t name_size;
    bool success = false;
    (void)user_data;
    if (preview == NULL || !g_host.initialized || source_path == NULL ||
        !c_host_path_is_within(source_path, APP_PAGES_FILE_MANAGER_ROOT) ||
        lstat(source_path, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        return false;
    }
    name = strrchr(source_path, '/');
    name = name == NULL ? source_path : name + 1;
    name_size = strlen(name);
    if (name_size <= 4 || strcmp(name + name_size - 4, ".pxa") != 0) return false;
    if (!c_host_verify_package_source(source_path, &buffers,
                                      &manifest) ||
        !c_host_copy_manifest_text(preview->app.id, sizeof(preview->app.id),
                                   manifest->app_id) ||
        !c_host_copy_manifest_text(preview->app.name, sizeof(preview->app.name),
                                   manifest->name) ||
        !c_host_copy_manifest_text(preview->app.version,
                                   sizeof(preview->app.version),
                                   manifest->version) ||
        !c_host_copy_manifest_text(preview->description,
                                   sizeof(preview->description),
                                   manifest->description)) {
        goto done;
    }
    for (uint16_t index = 0;
         index < manifest->permission_count &&
         preview->permission_count < APP_PAGES_PACKAGE_PREVIEW_PERMISSION_MAX;
         ++index) {
        const pxa_package_permission_t *permission = &manifest->permissions[index];
        app_pages_app_permission_t *target =
            &preview->permissions[preview->permission_count];
        if (!c_host_copy_manifest_text(target->name, sizeof(target->name),
                                       permission->name) ||
            !c_host_copy_manifest_text(target->scope, sizeof(target->scope),
                                       permission->scope)) {
            goto done;
        }
        target->required = permission->required != 0;
        ++preview->permission_count;
    }
    installed = c_host_find_app(preview->app.id);
    if (installed != NULL && installed->installed)
        preview->app.icon = load_icon(installed);
    success = true;
done:
    if (!success) app_pages_release_app_icon(&preview->app.icon);
    c_host_manifest_buffers_release(&buffers);
    return success;
}

static bool c_host_install_package_cb(const char *source_path, void *user_data) {
    c_host_manifest_buffers_t buffers = {0};
    pxa_package_manifest_t *manifest = NULL;
    pxa_c_host_app_t *app;
    pxa_c_host_app_t previous;
    struct stat metadata;
    char app_id[PXA_C_HOST_MAX_APP_ID];
    const char *name;
    size_t name_size;
    int created = 0;
    bool success = false;
    (void)user_data;

    if (!g_host.initialized || source_path == NULL ||
        !c_host_path_is_within(source_path, APP_PAGES_FILE_MANAGER_ROOT) ||
        lstat(source_path, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        return false;
    }
    name = strrchr(source_path, '/');
    name = name == NULL ? source_path : name + 1;
    name_size = strlen(name);
    if (name_size <= 4 || strcmp(name + name_size - 4, ".pxa") != 0) return false;
    if (!c_host_verify_package_source(source_path, &buffers,
                                      &manifest) ||
        manifest->app_id.size == 0 ||
        manifest->app_id.size >= sizeof(app_id)) {
        goto done;
    }
    memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
    app_id[manifest->app_id.size] = '\0';
    app = c_host_find_app(app_id);
    if (!c_host_policy_app_id_valid(app_id) || (app != NULL && app->built_in)) {
        goto done;
    }
    if (app == NULL) {
        if (g_host.app_count >= PXA_C_HOST_MAX_APPS) goto done;
        app = &g_host.apps[g_host.app_count++];
        memset(app, 0, sizeof(*app));
        snprintf(app->id, sizeof(app->id), "%s", app_id);
        app->enabled = 1;
        created = 1;
    } else {
        previous = *app;
    }
    snprintf(app->source, sizeof(app->source), "%s", source_path);
    app->staged = 0;
    success = c_host_install_app(app) != 0;
    if (success) {
        app->source[0] = '\0';
        app->staged = 0;
    } else if (created) {
        memset(app, 0, sizeof(*app));
        --g_host.app_count;
    } else {
        *app = previous;
    }

done:
    c_host_manifest_buffers_release(&buffers);
    if (success && g_system_catalog_changed != NULL)
        g_system_catalog_changed(g_system_callback_context);
    return success;
}

static int c_host_verify_installed_app(pxa_c_host_app_t *app) {
    c_host_manifest_buffers_t buffers = {0};
    pxa_package_manifest_t *manifest = NULL;
    char installed_root[PXA_C_HOST_MAX_PATH];
    int success;
    success = c_host_load_current_app(app, &buffers, &manifest,
                                      installed_root, sizeof(installed_root)) &&
              c_host_manifest_matches(manifest, app->id) &&
              memcmp(manifest->management_key_id, app->publisher_key_id,
                     PXA_PACKAGE_DIGEST_BYTES) == 0;
    c_host_manifest_buffers_release(&buffers);
    return success;
}

static int c_host_install_all(void) {
    size_t index;
    for (index = 0; index < g_host.app_count; ++index) {
        pxa_c_host_app_t *app = &g_host.apps[index];
        if (app->built_in && !c_host_install_app(app)) app->installed = 0;
    }
    return 1;
}

/* --- service registration ------------------------------------------------- */

static int c_host_register_services(void) {
    pxa_service_ops_t operations;
    memset(&operations, 0, sizeof(operations));
    operations.struct_size = sizeof(operations);
    operations.service_id = PXA_CLOCK_SERVICE_ID;
    operations.major = PXA_CORE_SERVICE_MAJOR;
    operations.minor = PXA_CORE_SERVICE_MINOR;
    operations.context = NULL;
    operations.control = c_host_clock_control;
    if (pxa_service_register(g_host.runtime, &operations) != PXA_STATUS_OK) {
        return 0;
    }
    if (pxa_storage_service_register(g_host.storage_service) !=
            PXA_STATUS_OK ||
        pxa_fs_service_register(g_host.fs_service) != PXA_STATUS_OK ||
        pxa_window_service_register(g_host.window_service) != PXA_STATUS_OK ||
        pxa_ui_service_register(g_host.ui_service) != PXA_STATUS_OK ||
        pxa_ipc_broker_register(g_host.ipc_broker) != PXA_STATUS_OK ||
        pxa_lease_service_register(g_host.lease_service) != PXA_STATUS_OK ||
        pxa_permission_service_register(g_host.permission_service) !=
            PXA_STATUS_OK ||
        pxa_scheduler_service_register(g_host.scheduler_service) !=
            PXA_STATUS_OK ||
        pxa_sensor_service_register(g_host.sensor_service) != PXA_STATUS_OK ||
        pxa_device_service_register(g_host.device_service) != PXA_STATUS_OK ||
        pxa_net_service_register(g_host.net_service) != PXA_STATUS_OK ||
        pxa_audio_service_register(g_host.audio_service) != PXA_STATUS_OK ||
        pxa_surface_service_register(g_host.surface_service) != PXA_STATUS_OK ||
        pxa_game_render_service_register(g_host.game_render_service) != PXA_STATUS_OK) {
        return 0;
    }
    return 1;
}

/* --- maintenance ---------------------------------------------------------- */

static void c_host_post_clock_tick(void) {
    uint8_t message[PXA_ENVELOPE_SIZE + 8];
    pxa_writer_t writer;
    uint8_t payload[8];
    pxa_status_t status;
    if (g_host.active_component == PXA_COMPONENT_INVALID ||
        g_host.clock_period_ms == 0) {
        return;
    }
    if (g_host.fake_now_us - g_host.clock_last_tick_us <
        (uint64_t)g_host.clock_period_ms * 1000u) {
        return;
    }
    g_host.clock_last_tick_us +=
        (uint64_t)g_host.clock_period_ms * 1000u;
    pxa_write_u64(payload, g_host.fake_now_us);
    pxa_writer_init(&writer, message, sizeof(message));
    status = pxa_writer_message(&writer, PXA_CLOCK_SERVICE_ID, PXA_CLOCK_TICK,
                                0, payload, sizeof(payload));
    if (status == PXA_STATUS_OK) {
        (void)pxa_event_post(g_host.runtime, g_host.active_component, message,
                             writer.size, 0, 0x000400008001ULL);
    }
}

static void c_host_maintenance(lv_timer_t *timer) {
    pxa_component_t affected[2];
    size_t count = 0;
    pxa_scheduler_entry_t due[PXA_C_HOST_MAX_JOBS];
    size_t due_count = 0;
    size_t index;
    if (timer == NULL) {
        /* Self-tests call maintenance directly and require deterministic time. */
        g_host.fake_now_us += 16000;
    } else {
        uint32_t now_ms = lv_tick_get();
        uint32_t elapsed_ms = now_ms - g_host.maintenance_last_tick_ms;
        g_host.maintenance_last_tick_ms = now_ms;
        g_host.fake_now_us += (uint64_t)elapsed_ms * 1000u;
    }
    c_host_post_clock_tick();
    if (pxa_lease_has_active(g_host.lease_service) &&
        pxa_lease_revoke_expired(g_host.lease_service, affected, 2, &count) ==
            PXA_STATUS_OK) {
        for (index = 0; index < count; ++index) {
            (void)pxa_wamr_engine_deliver_event(g_host.engine, g_host.runtime,
                                                affected[index]);
        }
    }
    if (pxa_sensor_has_active_subscriptions(g_host.sensor_service) &&
        pxa_sensor_poll(g_host.sensor_service, g_host.fake_now_us, affected, 2,
                        &count) == PXA_STATUS_OK) {
        for (index = 0; index < count; ++index) {
            (void)pxa_wamr_engine_deliver_event(g_host.engine, g_host.runtime,
                                                affected[index]);
        }
    }
    if (pxa_net_has_pending_requests(g_host.net_service) &&
        pxa_net_poll(g_host.net_service, affected, 2, &count) ==
            PXA_STATUS_OK) {
        for (index = 0; index < count; ++index) {
            (void)pxa_wamr_engine_deliver_event(g_host.engine, g_host.runtime,
                                                affected[index]);
        }
    }
    c_host_maintain_jobs();
    if (pxa_scheduler_has_pending(g_host.scheduler_service) &&
        pxa_scheduler_take_due(g_host.scheduler_service, due,
                               PXA_C_HOST_MAX_JOBS, &due_count) ==
            PXA_STATUS_OK) {
        for (index = 0; index < due_count; ++index) {
            uint8_t config[PXA_WAMR_ENGINE_MAX_CONFIG_BYTES];
            size_t config_size = 0;
            char component_id[65];
            size_t component_size = due[index].component_id_size;
            pxa_component_t job_component = PXA_COMPONENT_INVALID;
            uint64_t job_instance;
            uint64_t duration_us;
            int job_slot;
            if (component_size >= sizeof(component_id)) continue;
            memcpy(component_id, due[index].component_id, component_size);
            component_id[component_size] = '\0';
            job_slot = c_host_job_component_slot(
                (pxa_bytes_t){(const uint8_t *)component_id, component_size});
            if (job_slot < 0 ||
                g_host.active_jobs[job_slot] != PXA_COMPONENT_INVALID) {
                c_host_defer_work(&due[index]);
                continue;
            }
            duration_us = (uint64_t)due[index].max_execution_ms * 1000u;
            if (g_host.fake_now_us > UINT64_MAX - duration_us) continue;
            if (pxa_scheduler_encode_start_config(
                    &due[index],
                    (g_host.fake_now_us + duration_us) / 1000u,
                    config, sizeof(config), &config_size) != PXA_STATUS_OK) {
                continue;
            }
            job_instance = g_host.next_instance_id++;
            if (pxa_wamr_engine_set_config(
                    g_host.engine, job_instance,
                    (pxa_bytes_t){config, config_size}) != PXA_STATUS_OK) {
                c_host_defer_work(&due[index]);
                continue;
            }
            g_host.active_work[job_slot] = due[index];
            g_host.work_result[job_slot] = 0;
            g_host.activating_job_slot = (int16_t)job_slot;
            if (pxa_activation_activate(
                    g_host.coordinator,
                    (pxa_bytes_t){(const uint8_t *)component_id,
                                  component_size},
                    job_instance, &job_component) != PXA_STATUS_OK) {
                g_host.activating_job_slot = -1;
                memset(&g_host.active_work[job_slot], 0,
                       sizeof(g_host.active_work[job_slot]));
                g_host.work_result[job_slot] = 0;
                c_host_defer_work(&due[index]);
                continue;
            }
            g_host.activating_job_slot = -1;
            if (!c_host_track_active_component(job_component)) {
                (void)pxa_activation_deactivate(
                    g_host.coordinator,
                    (pxa_bytes_t){(const uint8_t *)component_id,
                                  component_size},
                    PXA_STOP_NORMAL);
                memset(&g_host.active_work[job_slot], 0,
                       sizeof(g_host.active_work[job_slot]));
                g_host.work_result[job_slot] = 0;
                c_host_defer_work(&due[index]);
                continue;
            }
            g_host.active_jobs[job_slot] = job_component;
            g_host.job_stop_at_us[job_slot] =
                g_host.fake_now_us + duration_us;
        }
    }
    c_host_drain_active_events();
    c_host_maintain_jobs();
}

/* --- pointer callback ------------------------------------------------------ */

static void c_host_on_ui_event(uint32_t surface, uint32_t node,
                               pxa_ui_event_kind_t kind, uint16_t flags,
                               const void *value, size_t value_size,
                               void *user_data) {
    uint64_t timestamp_us;
    (void)user_data;
    if (g_host.ui_service == NULL ||
        g_host.ui_component == PXA_COMPONENT_INVALID) return;
    ++g_host.ui_event_probe_count;
    if (kind == PXA_UI_EVENT_POINTER) ++g_host.pointer_probe_count;
    timestamp_us = pxa_lvgl_ui_event_timestamp_us(g_host.ui_adapter);
    if (timestamp_us == 0) timestamp_us = host_now_us(NULL);
    (void)pxa_ui_queue_event(
        g_host.ui_service, g_host.ui_component, surface, node, kind, flags,
        timestamp_us, value, value_size);
}

/* --- app_pages callbacks ------------------------------------------------------ */

static size_t c_host_catalog_cb(app_pages_app_t *apps, size_t capacity,
                                void *user_data) {
    (void)user_data;
    return simulator_pxa_catalog_list(apps, capacity, NULL);
}

static void c_host_launch_delayed(lv_timer_t *timer) {
    char *identity = lv_timer_get_user_data(timer);
    pxa_c_host_app_t *app;
    bool started;
    if (identity == NULL) return;
    app = c_host_find_app(identity);
    started = g_system_launch_request != NULL && app != NULL &&
              app->has_publisher_key_id
                  ? g_system_launch_request(g_system_callback_context,
                                            app->publisher_key_id, app->id)
                  : simulator_pxa_start(identity);
    if (!started) {
        app_pages_dismiss_app_launch();
        app_pages_post_toast("应用启动失败", 1600);
    }
    free(identity);
}

static bool c_host_launch_cb(const char *id, void *user_data) {
    char *identity;
    lv_timer_t *timer;
    size_t size;
    (void)user_data;
    if (id == NULL || id[0] == '\0') return false;
    size = strlen(id) + 1u;
    identity = malloc(size);
    if (identity == NULL) return false;
    memcpy(identity, id, size);
    /* Let the current input frame render the launch page before loading the
     * Package synchronously on the simulator's LVGL thread. */
    timer = lv_timer_create(c_host_launch_delayed, 20, identity);
    if (timer == NULL) {
        free(identity);
        return false;
    }
    lv_timer_set_repeat_count(timer, 1);
    return true;
}

static size_t c_host_manager_cb(app_pages_managed_app_t *apps, size_t capacity,
                                void *user_data) {
    (void)user_data;
    return simulator_pxa_managed_list(apps, capacity);
}

static bool c_host_action_cb(app_pages_app_action_t action, const char *id,
                             void *user_data) {
    bool changed;
    (void)user_data;
    changed = simulator_pxa_manage(action, id);
    if (changed && action != APP_PAGES_APP_ACTION_CLEAR_DATA &&
        g_system_catalog_changed != NULL) {
        g_system_catalog_changed(g_system_callback_context);
    }
    return changed;
}

static void c_host_copy_permission_text(char *output, size_t capacity,
                                        pxa_bytes_t value,
                                        const char *empty_value) {
    size_t size;
    if (output == NULL || capacity == 0) return;
    if (value.data == NULL || value.size == 0) {
        snprintf(output, capacity, "%s", empty_value);
        return;
    }
    size = value.size < capacity - 1 ? value.size : capacity - 1;
    while (size != 0 && (value.data[size] & 0xc0u) == 0x80u) --size;
    if (size == 0) {
        snprintf(output, capacity, "%s", empty_value);
        return;
    }
    memcpy(output, value.data, size);
    output[size] = '\0';
}

static void c_host_dismiss_permission_prompt(void) {
    if (!g_host.permission_prompt.active) return;
    app_pages_dismiss_runtime_permission_prompt(
        g_host.permission_prompt.prompt_id);
    memset(&g_host.permission_prompt, 0, sizeof(g_host.permission_prompt));
}

static pxa_status_t c_host_request_runtime_permission_prompt(
    void *context, pxa_component_t component, pxa_request_token_t request_token,
    pxa_bytes_t app_identity, pxa_bytes_t name, pxa_bytes_t scope) {
    app_pages_runtime_permission_prompt_t prompt;
    c_host_app_permission_t *permission;
    uint32_t prompt_id;
    (void)context;
    (void)app_identity;
    if (g_host.active_app == NULL || request_token == 0 ||
        (g_host.active_component != PXA_COMPONENT_INVALID &&
         component != g_host.active_component) ||
        g_host.permission_prompt.active) {
        return PXA_STATUS_BUSY;
    }
    permission = c_host_find_app_permission(g_host.active_app, name, scope);
    if (permission == NULL) return PXA_STATUS_DENIED;
    prompt_id = ++g_host.next_permission_prompt_id;
    if (prompt_id == 0) prompt_id = ++g_host.next_permission_prompt_id;
    memset(&prompt, 0, sizeof(prompt));
    prompt.prompt_id = prompt_id;
    snprintf(prompt.app_name, sizeof(prompt.app_name), "%s",
             g_host.active_app->name[0] == '\0' ? g_host.active_app->id
                                                  : g_host.active_app->name);
    c_host_copy_permission_text(prompt.permission_name,
                                sizeof(prompt.permission_name), name,
                                "未知权限");
    c_host_copy_permission_text(prompt.scope, sizeof(prompt.scope), scope,
                                "-");
    g_host.permission_prompt.active = 1;
    g_host.permission_prompt.prompt_id = prompt_id;
    g_host.permission_prompt.component = component;
    g_host.permission_prompt.request_token = request_token;
    g_host.permission_prompt.permission_index =
        (uint16_t)(permission - g_host.active_app->permissions);
    if (!app_pages_post_runtime_permission_prompt(&prompt)) {
        memset(&g_host.permission_prompt, 0, sizeof(g_host.permission_prompt));
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    return PXA_STATUS_OK;
}

static bool c_host_runtime_permission_response(uint32_t prompt_id,
                                               bool granted,
                                               void *user_data) {
    pxa_status_t status;
    pxa_component_t component;
    pxa_request_token_t request_token;
    (void)user_data;
    if (!g_host.permission_prompt.active ||
        g_host.permission_prompt.prompt_id != prompt_id ||
        g_host.permission_service == NULL) {
        return false;
    }
    component = g_host.permission_prompt.component;
    request_token = g_host.permission_prompt.request_token;
    status = pxa_permission_prompt_complete(
        g_host.permission_service, component, request_token,
        granted ? PXA_PERMISSION_ALLOW : PXA_PERMISSION_DENY);
    if (status != PXA_STATUS_OK) return false;
    memset(&g_host.permission_prompt, 0, sizeof(g_host.permission_prompt));
    return true;
}

static size_t c_host_permission_list_cb(const char *id,
                                        app_pages_app_permission_t *permissions,
                                        size_t capacity, void *user_data) {
    pxa_c_host_app_t *app = c_host_find_app(id);
    size_t count = 0;
    size_t index;
    (void)user_data;
    if (app == NULL) return 0;
    if (permissions == NULL && capacity == 0) {
        for (index = 0; index < app->permission_count; ++index) {
            if (app->permissions[index].declaration_index <
                g_host.declaration_count) {
                ++count;
            }
        }
        return count;
    }
    if (permissions == NULL || capacity == 0) return 0;
    for (index = 0; index < app->permission_count && count < capacity;
         ++index) {
        const c_host_app_permission_t *permission = &app->permissions[index];
        const pxa_permission_declaration_t *declaration;
        if (permission->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[permission->declaration_index];
        memset(&permissions[count], 0, sizeof(permissions[count]));
        c_host_copy_permission_text(permissions[count].name,
                                    sizeof(permissions[count].name),
                                    declaration->name, "未知权限");
        c_host_copy_permission_text(permissions[count].scope,
                                    sizeof(permissions[count].scope),
                                    declaration->scope, "");
        permissions[count].required = permission->required != 0;
        permissions[count].granted = permission->granted != 0;
        ++count;
    }
    return count;
}

static bool c_host_permission_set_cb(const char *id, size_t permission_index,
                                     bool granted, void *user_data) {
    pxa_c_host_app_t *app = c_host_find_app(id);
    c_host_app_permission_t *permission;
    const pxa_permission_declaration_t *declaration;
    pxa_status_t status;
    (void)user_data;
    if (app == NULL || permission_index >= app->permission_count) return false;
    permission = &app->permissions[permission_index];
    if (permission->declaration_index >= g_host.declaration_count) return false;
    declaration = &g_host.declarations[permission->declaration_index];
    if (app != g_host.active_app) {
        const uint8_t previous = permission->granted;
        permission->granted = granted;
        if (c_host_save_app_permission_policy(app) != PXA_STATUS_OK) {
            permission->granted = previous;
            return false;
        }
        return true;
    }
    if (g_host.permission_service == NULL) return false;
    if (granted) {
        status = pxa_permission_set(
            g_host.permission_service, declaration->name, declaration->scope,
            PXA_PERMISSION_ALLOW);
    } else {
        pxa_component_t affected[8];
        size_t affected_count = 0;
        status = pxa_permission_revoke(
            g_host.permission_service, declaration->name, declaration->scope,
            affected, sizeof(affected) / sizeof(affected[0]), &affected_count);
        if (status == PXA_STATUS_OK && g_host.permission_prompt.active &&
            g_host.permission_prompt.permission_index == permission_index) {
            c_host_dismiss_permission_prompt();
        }
    }
    return status == PXA_STATUS_OK;
}

static void c_host_permission_config(
    const pxa_c_host_app_t *app,
    pxa_permission_declaration_t
        declarations[PXA_C_HOST_MAX_APP_PERMISSIONS],
    pxa_permission_config_t *config) {
    uint16_t index;
    memset(declarations, 0,
           sizeof(*declarations) * PXA_C_HOST_MAX_APP_PERMISSIONS);
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    if (app == NULL) {
        config->app_identity =
            (pxa_bytes_t){(const uint8_t *)"simulator", 9};
    } else {
        config->app_identity =
            (pxa_bytes_t){(const uint8_t *)app->id, strlen(app->id)};
        for (index = 0; index < app->permission_count; ++index) {
            uint16_t declaration_index =
                app->permissions[index].declaration_index;
            if (declaration_index >= g_host.declaration_count) continue;
            declarations[config->declaration_count] =
                g_host.declarations[declaration_index];
            declarations[config->declaration_count].required =
                app->permissions[index].required;
            ++config->declaration_count;
        }
        config->declarations = declarations;
    }
    config->max_authorities = 16;
    config->max_pending_prompts = 1;
    config->prompt = c_host_request_runtime_permission_prompt;
    config->store.struct_size = sizeof(config->store);
    config->store.load = c_host_permission_load;
    config->store.save = c_host_permission_save;
}

static int c_host_rebind_permissions(const pxa_c_host_app_t *app) {
    pxa_permission_declaration_t
        declarations[PXA_C_HOST_MAX_APP_PERMISSIONS];
    pxa_permission_config_t config;
    pxa_permission_service_t *service = NULL;
    c_host_permission_config(app, declarations, &config);
    if (pxa_permission_service_init(
            g_host.permission_workspace, g_host.permission_workspace_size,
            g_host.runtime, &config, &service) != PXA_STATUS_OK ||
        service != g_host.permission_service ||
        pxa_permission_policy_load(service) != PXA_STATUS_OK) {
        return 0;
    }
    return 1;
}

static void c_host_stop_active(void) {
    c_host_dismiss_permission_prompt();
    if (g_host.coordinator != NULL) {
        (void)pxa_activation_deactivate_all(g_host.coordinator,
                                            PXA_STOP_NORMAL);
    }
    free(g_host.coordinator_workspace);
    free(g_host.plan_workspace);
    g_host.coordinator_workspace = NULL;
    g_host.plan_workspace = NULL;
    g_host.coordinator = NULL;
    pxa_lvgl_ui_reset(g_host.ui_adapter);
    g_host.active_manifest = NULL;
    c_host_clear_ui_assets();
    g_host.ui_component = PXA_COMPONENT_INVALID;
    g_host.active_component = PXA_COMPONENT_INVALID;
    c_host_clear_active_components();
    c_host_clear_active_jobs();
    g_host.active_app = NULL;
    g_host.clock_period_ms = 0;
}

static void c_host_stop_active_after_back(void) {
    if (g_host.scheduler_service != NULL &&
        (pxa_scheduler_has_pending(g_host.scheduler_service) ||
         c_host_has_active_jobs())) {
        /* Drop the main component, but keep pending and running jobs alive. */
        (void)pxa_activation_deactivate(
            g_host.coordinator, (pxa_bytes_t){(const uint8_t *)"main", 4},
            PXA_STOP_NORMAL);
        c_host_untrack_active_component(g_host.active_component);
        pxa_lvgl_ui_reset(g_host.ui_adapter);
        g_host.ui_component = PXA_COMPONENT_INVALID;
        g_host.active_component = PXA_COMPONENT_INVALID;
        return;
    }
    c_host_stop_active();
}

static int c_host_dispatch_back_request(uint8_t *close_requested) {
    pxa_wamr_event_result_t result;
    pxa_message_view_t event;
    pxa_status_t status;
    uint8_t attempt;
    if (close_requested == NULL || g_host.window_service == NULL ||
        g_host.active_component == PXA_COMPONENT_INVALID) {
        return 0;
    }
    status = pxa_window_queue_back(g_host.window_service,
                                   g_host.active_component);
    if (status != PXA_STATUS_OK) return 0;
    for (attempt = 0; attempt < PXA_C_HOST_BACK_EVENT_DRAIN_LIMIT;
         ++attempt) {
        status = pxa_wamr_engine_deliver_event_result(
            g_host.engine, g_host.runtime, g_host.active_component, &result);
        if (status != PXA_STATUS_OK) return -1;
        if (result.service != PXA_WINDOW_SERVICE_ID ||
            result.opcode != PXA_WINDOW_BACK_REQUESTED) {
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.service = result.service;
        event.opcode = result.opcode;
        event.request_token = result.request_token;
        event.payload.size = result.payload_size;
        return pxa_window_resolve_event_result(&event, result.guest_result,
                                               close_requested) == PXA_STATUS_OK
                   ? 1
                   : -1;
    }
    return 0;
}

int simulator_pxa_dispatch_back(void) {
    uint8_t close_requested = 1;
    int dispatched = c_host_dispatch_back_request(&close_requested);
    if (dispatched < 0) return -1;
    return dispatched > 0 && !close_requested ? 1 : 0;
}

static bool c_host_back_request_cb(void *user_data) {
    uint8_t close_requested = 1;
    int dispatched;
    (void)user_data;
    if (g_host.active_component == PXA_COMPONENT_INVALID) return false;
    dispatched = c_host_dispatch_back_request(&close_requested);
    if (dispatched > 0 && !close_requested) return true;
    if (g_system_close_request != NULL &&
        g_system_close_request(g_system_callback_context)) {
        return true;
    }
    c_host_stop_active_after_back();
    return true;
}

/* --- public interface -------------------------------------------------------- */

size_t simulator_pxa_catalog_list(app_pages_app_t *apps, size_t capacity,
                                  void *user_data) {
    size_t index;
    size_t count = 0;
    (void)user_data;
    if (apps == NULL && capacity == 0) {
        for (index = 0; index < g_host.app_count; ++index) {
            if (g_host.apps[index].installed) ++count;
        }
        return count;
    }
    if (apps == NULL || capacity == 0) return 0;
    for (index = 0; index < g_host.app_count && count < capacity; ++index) {
        pxa_c_host_app_t *app = &g_host.apps[index];
        if (!app->installed) continue;
        memset(&apps[count], 0, sizeof(apps[count]));
        snprintf(apps[count].id, sizeof(apps[count].id), "%s", app->id);
        snprintf(apps[count].name, sizeof(apps[count].name), "%s", app->name);
        snprintf(apps[count].version, sizeof(apps[count].version), "%s",
                 app->version);
        apps[count].icon = load_icon(app);
        apps[count].runnable = app->enabled != 0;
        ++count;
    }
    return count;
}

size_t simulator_pxa_system_list(simulator_pxa_system_app_t *apps,
                                 size_t capacity) {
    size_t index;
    size_t count = 0;
    if (apps == NULL && capacity == 0) {
        for (index = 0; index < g_host.app_count; ++index) {
            if (g_host.apps[index].installed &&
                g_host.apps[index].has_publisher_key_id) {
                ++count;
            }
        }
        return count;
    }
    if (apps == NULL || capacity == 0) return 0;
    for (index = 0; index < g_host.app_count && count < capacity; ++index) {
        const pxa_c_host_app_t *app = &g_host.apps[index];
        if (!app->installed || !app->has_publisher_key_id) continue;
        memset(&apps[count], 0, sizeof(apps[count]));
        memcpy(apps[count].publisher_root, app->publisher_key_id,
               sizeof(apps[count].publisher_root));
        snprintf(apps[count].app_id, sizeof(apps[count].app_id), "%s",
                 app->id);
        snprintf(apps[count].name, sizeof(apps[count].name), "%s",
                 app->name);
        snprintf(apps[count].version, sizeof(apps[count].version), "%s",
                 app->version);
        apps[count].enabled = app->enabled != 0;
        apps[count].built_in = app->built_in != 0;
        ++count;
    }
    return count;
}

static pxa_c_host_app_t *c_host_find_system_app(
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id) {
    size_t index;
    if (publisher_root == NULL || app_id == NULL) return NULL;
    for (index = 0; index < g_host.app_count; ++index) {
        pxa_c_host_app_t *app = &g_host.apps[index];
        if (app->installed && app->has_publisher_key_id &&
            strcmp(app->id, app_id) == 0 &&
            memcmp(app->publisher_key_id, publisher_root,
                   SIMULATOR_PXA_PUBLISHER_ROOT_BYTES) == 0) {
            return app;
        }
    }
    return NULL;
}

static int c_host_copy_metadata_text(char *output, size_t capacity,
                                     pxa_bytes_t value) {
    if (output == NULL || capacity == 0 || value.size >= capacity ||
        (value.size != 0 && value.data == NULL)) {
        return 0;
    }
    if (value.size != 0) memcpy(output, value.data, value.size);
    output[value.size] = '\0';
    return 1;
}

bool simulator_pxa_system_metadata(
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *locale,
    simulator_pxa_system_metadata_t *metadata) {
    pxa_c_host_app_t *app;
    c_host_manifest_buffers_t buffers = {0};
    pxa_package_manifest_t *manifest = NULL;
    pxa_package_metadata_t resolved;
    char installed_root[PXA_C_HOST_MAX_PATH];
    bool success = false;
    if (metadata == NULL || locale == NULL) return false;
    memset(metadata, 0, sizeof(*metadata));
    app = c_host_find_system_app(publisher_root, app_id);
    if (app == NULL || !c_host_load_current_app(
                           app, &buffers, &manifest, installed_root,
                           sizeof(installed_root)) ||
        pxa_package_metadata_resolve(
            manifest,
            (pxa_bytes_t){(const uint8_t *)locale, strlen(locale)},
            &resolved) != PXA_STATUS_OK) {
        goto done;
    }
    success = c_host_copy_metadata_text(metadata->name,
                                        sizeof(metadata->name), resolved.name) &&
              c_host_copy_metadata_text(metadata->description,
                                        sizeof(metadata->description),
                                        resolved.description) &&
              c_host_copy_metadata_text(metadata->icon_path,
                                        sizeof(metadata->icon_path),
                                        resolved.icon_path);
done:
    c_host_manifest_buffers_release(&buffers);
    return success;
}

app_pages_app_icon_t simulator_pxa_system_icon_path(
    const uint8_t publisher_root[SIMULATOR_PXA_PUBLISHER_ROOT_BYTES],
    const char *app_id, const char *icon_path) {
    pxa_c_host_app_t *app = c_host_find_system_app(publisher_root, app_id);
    pxa_c_host_app_t localized;
    if (app == NULL || icon_path == NULL ||
        strlen(icon_path) >= sizeof(localized.icon_path)) {
        return (app_pages_app_icon_t){0};
    }
    localized = *app;
    snprintf(localized.icon_path, sizeof(localized.icon_path), "%s",
             icon_path);
    return load_icon(&localized);
}

void simulator_pxa_set_system_callbacks(
    simulator_pxa_launch_request_fn launch_request,
    simulator_pxa_close_request_fn close_request,
    simulator_pxa_catalog_changed_fn catalog_changed,
    simulator_pxa_window_changed_fn window_changed, void *context) {
    g_system_launch_request = launch_request;
    g_system_close_request = close_request;
    g_system_catalog_changed = catalog_changed;
    g_system_window_changed = window_changed;
    g_system_callback_context = context;
    if (g_host.initialized && catalog_changed != NULL) catalog_changed(context);
}

size_t simulator_pxa_managed_list(app_pages_managed_app_t *apps,
                                  size_t capacity) {
    size_t index;
    size_t count = 0;
    c_host_manifest_buffers_t buffers = {0};
    c_host_scan_inbox();
    if (apps == NULL && capacity == 0) {
        for (index = 0; index < g_host.app_count; ++index) {
            if (g_host.apps[index].installed || g_host.apps[index].staged) {
                ++count;
            }
        }
        return count;
    }
    if (apps == NULL || capacity == 0) return 0;
    for (index = 0; index < g_host.app_count && count < capacity; ++index) {
        pxa_c_host_app_t *app = &g_host.apps[index];
        uint16_t installed_lineage_count = 0;
        uint16_t permission_index;
        uint16_t granted = 0;
        if (!app->installed && !app->staged) continue;
        memset(&apps[count], 0, sizeof(apps[count]));
        snprintf(apps[count].id, sizeof(apps[count].id), "%s", app->id);
        snprintf(apps[count].name, sizeof(apps[count].name), "%s", app->name);
        snprintf(apps[count].version, sizeof(apps[count].version), "%s",
                 app->version);
        apps[count].icon = load_icon(app);
        apps[count].enabled = app->enabled;
        apps[count].built_in = app->built_in != 0;
        apps[count].installed = app->installed != 0;
        apps[count].staged = app->staged != 0;
        apps[count].active = app == g_host.active_app;
        {
            char data_path[PXA_C_HOST_MAX_PATH];
            struct stat metadata;
            apps[count].has_private_data =
                c_host_app_data_path(data_path, sizeof(data_path), app) &&
                stat(data_path, &metadata) == 0 &&
                S_ISDIR(metadata.st_mode);
        }
        apps[count].permission_count =
            app->permission_count > UINT8_MAX ? UINT8_MAX
                                              : (uint8_t)app->permission_count;
        for (permission_index = 0; permission_index < app->permission_count;
             ++permission_index) {
            if (app->permissions[permission_index].granted) ++granted;
        }
        apps[count].granted_permission_count =
            granted > UINT8_MAX ? UINT8_MAX : (uint8_t)granted;
        {
            pxa_package_manifest_t *manifest = NULL;
            char installed_root[PXA_C_HOST_MAX_PATH];
            if (app->installed && c_host_load_current_app(
                                      app, &buffers, &manifest,
                                      installed_root,
                                      sizeof(installed_root))) {
                pxa_package_publisher_lineage_t lineage;
                apps[count].has_release_sequence =
                    manifest->has_release_sequence;
                apps[count].release_sequence = manifest->release_sequence;
                if (pxa_package_publisher_lineage_parse(manifest, &lineage) ==
                    PXA_STATUS_OK) {
                    installed_lineage_count = lineage.link_count;
                }
            }
            manifest = NULL;
            if (app->staged && c_host_verify_package_source(
                                   app->source, &buffers, &manifest)) {
                pxa_package_publisher_lineage_t lineage;
                size_t version_size =
                    manifest->version.size < sizeof(apps[count].staged_version) - 1
                        ? manifest->version.size
                        : sizeof(apps[count].staged_version) - 1;
                memcpy(apps[count].staged_version, manifest->version.data,
                       version_size);
                apps[count].staged_version[version_size] = '\0';
                apps[count].has_staged_release_sequence =
                    manifest->has_release_sequence;
                apps[count].staged_release_sequence =
                    manifest->release_sequence;
                if (app->installed) {
                    apps[count].downgrade_warning =
                        apps[count].has_release_sequence &&
                        (!apps[count].has_staged_release_sequence ||
                         apps[count].staged_release_sequence <
                             apps[count].release_sequence);
                    if (pxa_package_publisher_lineage_parse(
                            manifest, &lineage) == PXA_STATUS_OK &&
                        lineage.link_count < installed_lineage_count) {
                        apps[count].signer_rollback_warning = true;
                    }
                }
            }
        }
        ++count;
    }
    c_host_manifest_buffers_release(&buffers);
    return count;
}

bool simulator_pxa_manage(app_pages_app_action_t action, const char *app_id) {
    size_t index;
    if (!g_host.initialized || app_id == NULL) return false;
    for (index = 0; index < g_host.app_count; ++index) {
        pxa_c_host_app_t *app = &g_host.apps[index];
        if (strcmp(app->id, app_id) != 0) continue;
        if (app == g_host.active_app) return false;
        switch (action) {
            case APP_PAGES_APP_ACTION_INSTALL:
                if (!app->staged) return app->installed != 0;
                return c_host_install_app(app) != 0;
            case APP_PAGES_APP_ACTION_UNINSTALL: {
                pxa_posix_installer_identity_t identity;
                if (app->built_in || !app->installed ||
                    !app->has_publisher_key_id ||
                    !c_host_verify_installed_app(app)) {
                    return false;
                }
                memset(&identity, 0, sizeof(identity));
                identity.app_id = (pxa_bytes_t){
                    (const uint8_t *)app->id, strlen(app->id)};
                identity.publisher_key_id = (pxa_bytes_t){
                    app->publisher_key_id, PXA_PACKAGE_DIGEST_BYTES};
                if (pxa_posix_installer_uninstall(g_host.installer,
                                                  &identity) !=
                    PXA_STATUS_OK) {
                    return false;
                }
                app->installed = 0;
                app->has_publisher_key_id = 0;
                app->permission_count = 0;
                app->job_component_count = 0;
                app->root[0] = '\0';
                c_host_scan_inbox();
                return true;
            }
            case APP_PAGES_APP_ACTION_CLEAR_DATA: {
                char data_path[PXA_C_HOST_MAX_PATH];
                pxa_status_t status;
                if (!app->installed ||
                    !c_host_app_data_path(data_path, sizeof(data_path), app)) {
                    return false;
                }
                status = pxa_posix_fs_remove_tree(data_path);
                return status == PXA_STATUS_OK ||
                       status == PXA_STATUS_NOT_FOUND;
            }
            case APP_PAGES_APP_ACTION_ENABLE:
            case APP_PAGES_APP_ACTION_DISABLE: {
                size_t disabled_index = c_host_disabled_app_index(app->id);
                if (!app->installed) return false;
                if (action == APP_PAGES_APP_ACTION_ENABLE) {
                    if (disabled_index == SIZE_MAX) {
                        app->enabled = 1;
                        return true;
                    }
                    memmove(&g_host.disabled_apps[disabled_index],
                            &g_host.disabled_apps[disabled_index + 1u],
                            (g_host.disabled_app_count - disabled_index - 1u) *
                                sizeof(g_host.disabled_apps[0]));
                    --g_host.disabled_app_count;
                    if (!c_host_save_disabled_policy()) {
                        memmove(&g_host.disabled_apps[disabled_index + 1u],
                                &g_host.disabled_apps[disabled_index],
                                (g_host.disabled_app_count - disabled_index) *
                                    sizeof(g_host.disabled_apps[0]));
                        snprintf(g_host.disabled_apps[disabled_index],
                                 sizeof(g_host.disabled_apps[0]), "%s",
                                 app->id);
                        ++g_host.disabled_app_count;
                        return false;
                    }
                    app->enabled = 1;
                    return true;
                }
                if (disabled_index != SIZE_MAX) {
                    app->enabled = 0;
                    return true;
                }
                if (g_host.disabled_app_count >= PXA_C_HOST_MAX_APPS) {
                    return false;
                }
                snprintf(g_host.disabled_apps[g_host.disabled_app_count],
                         sizeof(g_host.disabled_apps[0]), "%s", app->id);
                ++g_host.disabled_app_count;
                if (!c_host_save_disabled_policy()) {
                    --g_host.disabled_app_count;
                    memset(g_host.disabled_apps[g_host.disabled_app_count], 0,
                           sizeof(g_host.disabled_apps[0]));
                    return false;
                }
                app->enabled = 0;
                return true;
            }
            default:
                return false;
        }
    }
    return false;
}

static int c_host_state_path(char* output, size_t capacity, const char* relative) {
    int size;
    if (output == NULL || capacity == 0 || relative == NULL)
        return 0;
    size = snprintf(output, capacity, "%s/%s", g_host.storage_root, relative);
    return size > 0 && (size_t)size < capacity;
}

static int c_host_initialize_runtime(void) {
    pxa_runtime_limits_t limits;
    size_t workspace_size;
    pxa_runtime_limits_init(&limits);
    limits.max_components = 8;
    workspace_size = pxa_runtime_workspace_size(&limits);
    g_host.runtime_workspace = malloc(workspace_size);
    return g_host.runtime_workspace != NULL &&
           pxa_runtime_init(g_host.runtime_workspace, workspace_size, &limits, &g_host.runtime) ==
               PXA_STATUS_OK;
}

static int c_host_initialize_storage(void) {
    static const uint8_t scheduler_key[] = "work.v1";
    pxa_posix_storage_config_t adapter_config;
    pxa_posix_scheduler_store_config_t scheduler_config;
    char root[PXA_C_HOST_MAX_PATH];
    size_t workspace_size;
    memset(&adapter_config, 0, sizeof(adapter_config));
    adapter_config.struct_size = sizeof(adapter_config);
    if (!c_host_state_path(root, sizeof(root), "data/.pxa-unbound/.pxa-storage") ||
        !make_dirs(root)) {
        return 0;
    }
    adapter_config.root_path = root;
    adapter_config.max_keys = 32;
    adapter_config.max_value_bytes = 960;
    adapter_config.quota_bytes = 8192;
    workspace_size = pxa_posix_storage_workspace_size(&adapter_config);
    g_host.storage_workspace = malloc(workspace_size);
    if (g_host.storage_workspace == NULL ||
        pxa_posix_storage_init(g_host.storage_workspace, workspace_size, &adapter_config,
                               &g_host.posix_storage, &g_host.storage_backend) != PXA_STATUS_OK) {
        return 0;
    }

    memset(&scheduler_config, 0, sizeof(scheduler_config));
    scheduler_config.struct_size = sizeof(scheduler_config);
    scheduler_config.backend = g_host.storage_backend;
    scheduler_config.key = (pxa_bytes_t){scheduler_key, sizeof(scheduler_key) - 1u};
    scheduler_config.max_entries = 8;
    scheduler_config.epoch =
        SDL_GetPerformanceCounter() ^ ((uint64_t)(uint32_t)getpid() << 32);
    if (scheduler_config.epoch == 0) scheduler_config.epoch = 1;
    workspace_size = pxa_posix_scheduler_store_workspace_size(&scheduler_config);
    g_host.scheduler_store_workspace = malloc(workspace_size);
    if (g_host.scheduler_store_workspace == NULL ||
        pxa_posix_scheduler_store_init(g_host.scheduler_store_workspace, workspace_size,
                                       &scheduler_config, &g_host.posix_scheduler_store,
                                       &g_host.scheduler_store) != PXA_STATUS_OK) {
        return 0;
    }

    memset(&g_host.storage_config, 0, sizeof(g_host.storage_config));
    g_host.storage_config.struct_size = sizeof(g_host.storage_config);
    g_host.storage_config.max_value_bytes = 960;
    g_host.storage_config.backend = g_host.storage_backend;
    workspace_size = pxa_storage_service_workspace_size(&g_host.storage_config);
    g_host.storage_service_workspace = malloc(workspace_size);
    return g_host.storage_service_workspace != NULL &&
           pxa_storage_service_init(g_host.storage_service_workspace, workspace_size,
                                    g_host.runtime, &g_host.storage_config,
                                    &g_host.storage_service) == PXA_STATUS_OK;
}

static int c_host_initialize_fs(void) {
    pxa_posix_fs_config_t adapter_config;
    char root[PXA_C_HOST_MAX_PATH];
    size_t workspace_size;
    memset(&adapter_config, 0, sizeof(adapter_config));
    adapter_config.struct_size = sizeof(adapter_config);
    if (!c_host_state_path(root, sizeof(root), "data/.pxa-unbound") || !make_dirs(root)) {
        return 0;
    }
    adapter_config.root_path = root;
    adapter_config.quota_bytes = 4ull * 1024 * 1024;
    adapter_config.max_open_resources = 16;
    workspace_size = pxa_posix_fs_workspace_size(&adapter_config);
    g_host.fs_service_workspace = malloc(workspace_size);
    if (g_host.fs_service_workspace == NULL ||
        pxa_posix_fs_init(g_host.fs_service_workspace, workspace_size, &adapter_config,
                          &g_host.posix_fs, &g_host.fs_backend) != PXA_STATUS_OK) {
        return 0;
    }
    memset(&g_host.fs_config, 0, sizeof(g_host.fs_config));
    g_host.fs_config.struct_size = sizeof(g_host.fs_config);
    g_host.fs_config.max_open_resources = 16;
    g_host.fs_config.backend = g_host.fs_backend;
    workspace_size = pxa_fs_service_workspace_size(&g_host.fs_config);
    g_host.fs_service_state_workspace = malloc(workspace_size);
    return g_host.fs_service_state_workspace != NULL &&
           pxa_fs_service_init(g_host.fs_service_state_workspace, workspace_size, g_host.runtime,
                               &g_host.fs_config, &g_host.fs_service) == PXA_STATUS_OK;
}

static int c_host_initialize_core_services(void) {
    pxa_ipc_limits_t ipc_limits;
    pxa_lease_limits_t lease_limits;
    pxa_ui_config_t ui_config;
    size_t workspace_size;
    workspace_size = pxa_window_service_workspace_size(8);
    g_host.window_workspace = malloc(workspace_size);
    if (g_host.window_workspace == NULL ||
        pxa_window_service_init(g_host.window_workspace, workspace_size, g_host.runtime, 8,
                                &g_host.window_service) != PXA_STATUS_OK) {
        return 0;
    }
    pxa_ui_config_init(&ui_config);
    ui_config.allocator_context = NULL;
    ui_config.allocate = c_host_ui_alloc;
    ui_config.release = c_host_ui_free;
    ui_config.now_us = host_now_us;
    ui_config.max_dynamic_bytes = g_host.ui_stress_profile
                                      ? 4u * 1024u * 1024u
                                      : 1024u * 1024u;
    ui_config.max_transaction_bytes = g_host.ui_stress_profile
                                          ? 1024u * 1024u
                                          : 256u * 1024u;
    ui_config.max_canvas_bytes = g_host.ui_stress_profile
                                     ? 1024u * 1024u
                                     : 256u * 1024u;
    ui_config.features = PXA_UI_FEATURE_CANVAS |
                         PXA_UI_FEATURE_VIRTUAL_LIST |
                         PXA_UI_FEATURE_RGB565_BITMAP |
                         PXA_UI_FEATURE_CONTROLLER_INPUT |
                         PXA_UI_FEATURE_CANVAS_STREAM_IO;
    ui_config.primary_width = 320;
    ui_config.primary_height = 240;
    workspace_size = pxa_ui_service_workspace_size();
    g_host.ui_workspace = malloc(workspace_size);
    if (g_host.ui_workspace == NULL ||
        pxa_ui_service_init(g_host.ui_workspace, workspace_size,
                               g_host.runtime, &ui_config,
                               &g_host.ui_service) != PXA_STATUS_OK) {
        return 0;
    }
    pxa_ipc_limits_init(&ipc_limits);
    ipc_limits.max_endpoints = 16;
    ipc_limits.max_pending_calls = 16;
    workspace_size = pxa_ipc_broker_workspace_size(&ipc_limits);
    g_host.ipc_broker_workspace = malloc(workspace_size);
    if (g_host.ipc_broker_workspace == NULL ||
        pxa_ipc_broker_init(g_host.ipc_broker_workspace, workspace_size, g_host.runtime,
                            &ipc_limits, &g_host.ipc_broker) != PXA_STATUS_OK) {
        return 0;
    }
    pxa_lease_limits_init(&lease_limits);
    lease_limits.allowed_kinds = (UINT32_C(1) << 6) - 1u;
    lease_limits.max_leases = 8;
    lease_limits.max_leases_per_component = 4;
    lease_limits.default_duration_ms = 5000;
    lease_limits.max_duration_ms = 60000;
    lease_limits.clock = c_host_scheduler_clock_ms;
    workspace_size = pxa_lease_service_workspace_size(&lease_limits);
    g_host.lease_workspace = malloc(workspace_size);
    return g_host.lease_workspace != NULL &&
           pxa_lease_service_init(g_host.lease_workspace, workspace_size, g_host.runtime,
                                  &lease_limits, &g_host.lease_service) == PXA_STATUS_OK;
}

static int c_host_initialize_permission(void) {
    static pxa_permission_declaration_t capacity_declarations[PXA_C_HOST_MAX_APP_PERMISSIONS];
    static uint8_t capacity_names[PXA_C_HOST_MAX_APP_PERMISSIONS]
                                 [PXA_C_HOST_PERMISSION_NAME_MAX - 1];
    static uint8_t capacity_scopes[PXA_C_HOST_MAX_APP_PERMISSIONS]
                                  [PXA_C_HOST_PERMISSION_SCOPE_MAX - 1];
    static uint8_t capacity_identity[PXA_C_HOST_MAX_APP_ID - 1];
    pxa_permission_declaration_t declarations[PXA_C_HOST_MAX_APP_PERMISSIONS];
    pxa_permission_config_t config;
    pxa_permission_config_t capacity_config;
    uint16_t index;
    c_host_permission_config(NULL, declarations, &config);
    c_host_permission_config(NULL, declarations, &capacity_config);
    memset(capacity_identity, 'a', sizeof(capacity_identity));
    capacity_config.app_identity = (pxa_bytes_t){capacity_identity, sizeof(capacity_identity)};
    for (index = 0; index < PXA_C_HOST_MAX_APP_PERMISSIONS; ++index) {
        memset(capacity_names[index], 'a', sizeof(capacity_names[index]));
        capacity_names[index][sizeof(capacity_names[index]) - 1u] = (uint8_t)('a' + index % 26u);
        capacity_names[index][sizeof(capacity_names[index]) - 2u] = (uint8_t)('a' + index / 26u);
        capacity_declarations[index].name =
            (pxa_bytes_t){capacity_names[index], sizeof(capacity_names[index])};
        capacity_declarations[index].scope =
            (pxa_bytes_t){capacity_scopes[index], sizeof(capacity_scopes[index])};
    }
    capacity_config.declarations = capacity_declarations;
    capacity_config.declaration_count = PXA_C_HOST_MAX_APP_PERMISSIONS;
    g_host.permission_workspace_size = pxa_permission_service_workspace_size(&capacity_config);
    g_host.permission_workspace = malloc(g_host.permission_workspace_size);
    return g_host.permission_workspace != NULL &&
           pxa_permission_service_init(g_host.permission_workspace,
                                       g_host.permission_workspace_size, g_host.runtime, &config,
                                       &g_host.permission_service) == PXA_STATUS_OK &&
           pxa_permission_policy_load(g_host.permission_service) == PXA_STATUS_OK;
}

static int c_host_initialize_scheduler(void) {
    pxa_scheduler_config_t config;
    pxa_bytes_t capacity_jobs[PXA_C_HOST_MAX_JOBS];
    char names[PXA_C_HOST_MAX_JOBS][24];
    uint16_t index;
    size_t workspace_size;
    for (index = 0; index < PXA_C_HOST_MAX_JOBS; ++index) {
        int name_size =
            snprintf(names[index], sizeof(names[index]), "reserved.%u", (unsigned)index);
        if (name_size <= 0 || (size_t)name_size >= sizeof(names[index])) {
            return 0;
        }
        capacity_jobs[index] = (pxa_bytes_t){(const uint8_t*)names[index], (size_t)name_size};
    }
    pxa_scheduler_config_init(&config);
    config.job_components = capacity_jobs;
    config.job_component_count = PXA_C_HOST_MAX_JOBS;
    config.max_entries = 8;
    config.clock = c_host_scheduler_clock_ms;
    config.store = g_host.scheduler_store;
    config.work_context = NULL;
    config.complete_work = c_host_complete_work;
    config.cancel_work = c_host_cancel_work;
    workspace_size = pxa_scheduler_service_workspace_size(&config);
    g_host.scheduler_workspace = malloc(workspace_size);
    return g_host.scheduler_workspace != NULL &&
           pxa_scheduler_service_init(g_host.scheduler_workspace, workspace_size, g_host.runtime,
                                      &config, &g_host.scheduler_service) == PXA_STATUS_OK &&
           pxa_scheduler_load(g_host.scheduler_service) == PXA_STATUS_OK;
}

static int c_host_initialize_providers(void) {
    pxa_sensor_descriptor_t descriptors[2];
    pxa_sensor_config_t sensor_config;
    pxa_device_config_t device_config;
    pxa_net_backend_t net_backend;
    pxa_net_config_t net_config;
    pxa_audio_backend_t audio_backend;
    pxa_audio_config_t audio_config;
    pxa_surface_backend_t surface_backend;
    pxa_surface_config_t surface_config;
    size_t workspace_size;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 0;
    g_host.curl_initialized = 1;
    g_host.curl_multi = curl_multi_init();
    if (g_host.curl_multi == NULL) return 0;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].id = 1;
    descriptors[0].semantic = (pxa_bytes_t){(const uint8_t*)"ambient.temperature", 18};
    descriptors[0].unit = PXA_SENSOR_UNIT_MILLI_CELSIUS;
    descriptors[0].dimensions = 1;
    descriptors[0].min_period_ms = 1000;
    descriptors[0].max_period_ms = 60000;
    descriptors[1].id = 2;
    descriptors[1].semantic = (pxa_bytes_t){(const uint8_t*)"ambient.light", 13};
    descriptors[1].unit = PXA_SENSOR_UNIT_MILLI_LUX;
    descriptors[1].dimensions = 1;
    descriptors[1].min_period_ms = 1000;
    descriptors[1].max_period_ms = 60000;
    memset(&sensor_config, 0, sizeof(sensor_config));
    sensor_config.struct_size = sizeof(sensor_config);
    sensor_config.descriptors = descriptors;
    sensor_config.descriptor_count = 2;
    sensor_config.max_subscriptions = 8;
    sensor_config.max_subscriptions_per_component = 4;
    sensor_config.subscribe = c_host_sensor_subscribe;
    sensor_config.read = c_host_sensor_read;
    sensor_config.unsubscribe = c_host_sensor_unsubscribe;
    sensor_config.permissions = g_host.permission_service;
    workspace_size = pxa_sensor_service_workspace_size(&sensor_config);
    g_host.sensor_workspace = malloc(workspace_size);
    if (g_host.sensor_workspace == NULL ||
        pxa_sensor_service_init(g_host.sensor_workspace, workspace_size, g_host.runtime,
                                &sensor_config, &g_host.sensor_service) != PXA_STATUS_OK) {
        return 0;
    }

    memset(&device_config, 0, sizeof(device_config));
    device_config.struct_size = sizeof(device_config);
    device_config.get_mac = c_host_device_get_mac;
    device_config.permissions = g_host.permission_service;
    workspace_size = pxa_device_service_workspace_size(&device_config);
    g_host.device_workspace = malloc(workspace_size);
    if (g_host.device_workspace == NULL ||
        pxa_device_service_init(g_host.device_workspace, workspace_size,
                                g_host.runtime, &device_config,
                                &g_host.device_service) != PXA_STATUS_OK) {
        return 0;
    }

    memset(&net_backend, 0, sizeof(net_backend));
    net_backend.struct_size = sizeof(net_backend);
    net_backend.context = &g_host;
    net_backend.start = c_host_net_start;
    net_backend.poll = c_host_net_poll;
    net_backend.cancel = c_host_net_cancel;
    net_backend.read_body = c_host_net_read_body;
    net_backend.close_body = c_host_net_close_body;
    memset(&net_config, 0, sizeof(net_config));
    net_config.struct_size = sizeof(net_config);
    net_config.max_pending_requests = 4;
    net_config.max_requests_per_component = 2;
    net_config.max_response_streams = 4;
    net_config.max_response_bytes = 4096;
    net_config.max_headers = 8;
    net_config.max_inline_body_bytes = 2048;
    net_config.max_request_header_bytes = 2048;
    net_config.max_response_header_bytes = 2048;
    net_config.min_timeout_ms = 100;
    net_config.default_timeout_ms = 15000;
    net_config.max_timeout_ms = 60000;
    net_config.backend = net_backend;
    net_config.permissions = g_host.permission_service;
    workspace_size = pxa_net_service_workspace_size(&net_config);
    g_host.net_workspace = malloc(workspace_size);
    if (g_host.net_workspace == NULL ||
        pxa_net_service_init(g_host.net_workspace, workspace_size, g_host.runtime, &net_config,
                             &g_host.net_service) != PXA_STATUS_OK) {
        return 0;
    }

    memset(&audio_backend, 0, sizeof(audio_backend));
    audio_backend.struct_size = sizeof(audio_backend);
    audio_backend.context = &g_host;
    audio_backend.open = c_host_audio_open;
    audio_backend.commit = c_host_audio_commit;
    audio_backend.submit = c_host_audio_submit;
    audio_backend.close = c_host_audio_close;
    audio_backend.query = c_host_audio_query;
    audio_backend.flush = c_host_audio_flush;
    audio_backend.play_tone = c_host_audio_play_tone;
    audio_backend.play_asset = c_host_audio_play_asset;
    audio_backend.control_asset = c_host_audio_control_asset;
    memset(&audio_config, 0, sizeof(audio_config));
    audio_config.struct_size = sizeof(audio_config);
    audio_config.max_sessions = 4;
    audio_config.max_sessions_per_component = 2;
    audio_config.max_eq_bands = 4;
    audio_config.backend = audio_backend;
    audio_config.permissions = g_host.permission_service;
    workspace_size = pxa_audio_service_workspace_size(&audio_config);
    g_host.audio_workspace = malloc(workspace_size);
    if (g_host.audio_workspace == NULL ||
        pxa_audio_service_init(g_host.audio_workspace, workspace_size,
                               g_host.runtime, &audio_config,
                               &g_host.audio_service) != PXA_STATUS_OK)
        return 0;

    memset(&surface_backend, 0, sizeof(surface_backend));
    surface_backend.struct_size = sizeof(surface_backend);
    surface_backend.context = &g_host;
    surface_backend.create = c_host_surface_create;
    surface_backend.write = c_host_surface_write;
    surface_backend.queue = c_host_surface_queue;
    surface_backend.configure = c_host_surface_configure;
    surface_backend.configure_opaque_ui_regions = c_host_surface_configure_opaque_ui_regions;
    surface_backend.query = c_host_surface_query;
    surface_backend.close = c_host_surface_close;
    surface_backend.register_buffers = c_host_surface_register_buffers;
    surface_backend.acquire_buffer = c_host_surface_acquire_buffer;
    surface_backend.present_buffer = c_host_surface_present_buffer;
    surface_backend.peek_release = c_host_surface_peek_release;
    surface_backend.consume_release = c_host_surface_consume_release;
    memset(&surface_config, 0, sizeof(surface_config));
    surface_config.struct_size = sizeof(surface_config);
    surface_config.max_surfaces = 1;
    surface_config.max_surfaces_per_component = 1;
    surface_config.max_width = g_display_metrics.width;
    surface_config.max_height = g_display_metrics.height;
    surface_config.max_frame_bytes =
        g_display_metrics.width * g_display_metrics.height * 4u;
    surface_config.min_buffer_count = 2;
    surface_config.max_buffer_count = 3;
    surface_config.backend = surface_backend;
    workspace_size = pxa_surface_service_workspace_size(&surface_config);
    g_host.surface_workspace = malloc(workspace_size);
    if (g_host.surface_workspace == NULL || pxa_surface_service_init(
            g_host.surface_workspace, workspace_size, g_host.runtime,
            &surface_config, &g_host.surface_service) != PXA_STATUS_OK) return 0;
    pxa_game_render_config_t game = {0};
    game.struct_size = sizeof(game);
    game.max_contexts = game.max_contexts_per_component = 1;
    game.min_buffer_count = 2; game.max_buffer_count = 3;
    game.auto_target_profile.display_width = g_display_metrics.width;
    game.auto_target_profile.display_height = g_display_metrics.height;
    game.auto_target_profile.supported_scale_mask = PXA_GAME_RENDER_KNOWN_SCALE_MASK;
    game.auto_target_profile.default_scale = 2;
    game.backend.struct_size = sizeof(game.backend);
    game.backend.context = &g_host;
    game.backend.create = c_host_game_render_create;
    game.backend.upload = c_host_surface_raster_upload;
    game.backend.submit = c_host_surface_raster_submit;
    game.backend.query = c_host_surface_raster_query;
    game.backend.close = c_host_surface_close;
    workspace_size = pxa_game_render_service_workspace_size(&game);
    g_host.game_render_workspace = malloc(workspace_size);
    return g_host.game_render_workspace != NULL && pxa_game_render_service_init(
        g_host.game_render_workspace, workspace_size, g_host.runtime, &game,
        &g_host.game_render_service) == PXA_STATUS_OK;
}

static int c_host_initialize_execution(void) {
    pxa_lvgl_ui_config_t ui_config;
    pxa_wamr_engine_config_t engine_config;
    size_t workspace_size;
    memset(&ui_config, 0, sizeof(ui_config));
    ui_config.struct_size = sizeof(ui_config);
    ui_config.allocate = c_host_ui_alloc;
    ui_config.release = c_host_ui_free;
    ui_config.execute = c_host_sync_execute;
    pxa_lvgl_ui_theme_init(&ui_config.theme);
    ui_config.theme.caption_font = app_pages_get_text_font();
    ui_config.theme.body_font = app_pages_get_text_font();
    ui_config.theme.title_font = app_pages_get_title_font();
    ui_config.theme.icon_font = &font_awesome_20_4;
    ui_config.resolve_asset = c_host_resolve_ui_asset;
    ui_config.release_asset = c_host_release_ui_asset_reference;
    ui_config.event_callback = c_host_on_ui_event;
    ui_config.now_us = host_now_us;
    ui_config.primary_environment.surface = PXA_UI_PRIMARY_SURFACE;
    ui_config.primary_environment.width = g_display_metrics.width;
    ui_config.primary_environment.height = g_display_metrics.height;
    ui_config.primary_environment.safe_insets[0] = g_display_metrics.safe_insets[0];
    ui_config.primary_environment.safe_insets[1] = g_display_metrics.safe_insets[1];
    ui_config.primary_environment.safe_insets[2] = g_display_metrics.safe_insets[2];
    ui_config.primary_environment.safe_insets[3] = g_display_metrics.safe_insets[3];
    ui_config.primary_environment.density_q16 = UINT32_C(1) << 16;
    ui_config.primary_environment.font_scale_q16 = UINT32_C(1) << 16;
    ui_config.primary_environment.features =
        PXA_UI_FEATURE_CANVAS | PXA_UI_FEATURE_VIRTUAL_LIST |
        PXA_UI_FEATURE_RGB565_BITMAP | PXA_UI_FEATURE_CONTROLLER_INPUT;
    workspace_size = pxa_lvgl_ui_workspace_size();
    g_host.ui_adapter_workspace = malloc(workspace_size);
    if (g_host.ui_adapter_workspace == NULL ||
        pxa_lvgl_ui_init(g_host.ui_adapter_workspace, workspace_size,
                            &ui_config, &g_host.ui_adapter,
                            &g_host.ui_backend) != PXA_STATUS_OK) {
        return 0;
    }
    memset(&engine_config, 0, sizeof(engine_config));
    engine_config.struct_size = sizeof(engine_config);
    engine_config.read_artifact = read_artifact;
    engine_config.now_us = host_now_us;
    engine_config.prepare_start = c_host_prepare_start;
    engine_config.guest_stack_size = 512 * 1024;
    engine_config.max_components = 8;
    engine_config.wasi_enabled = 1;
    /* ABI Lab activates its UI plus three IPC services concurrently. */
    engine_config.pool_bytes = 4 * 1024 * 1024;
    engine_config.max_module_bytes = 1024 * 1024;
    engine_config.allocate_artifact = c_host_artifact_alloc;
    engine_config.release_artifact = c_host_artifact_free;
    workspace_size = pxa_wamr_engine_workspace_size(&engine_config);
    g_host.engine_workspace = malloc(workspace_size);
    if (g_host.engine_workspace == NULL ||
        pxa_wamr_engine_init(g_host.engine_workspace, workspace_size, &engine_config,
                             &g_host.engine, &g_host.engine_ops) != PXA_STATUS_OK) {
        return 0;
    }
    pxa_wamr_engine_set_runtime(g_host.engine, g_host.runtime);
    return 1;
}

static int c_host_initialize_installer(void) {
    pxa_posix_installer_config_t config;
    FILE* input = fopen(PXA_SIMULATOR_PUBLISHER_KEY, "rb");
    size_t installer_size;
    int trailing;
    int read_failed;
    if (input == NULL)
        return 0;
    g_host.publisher_key_size = fread(g_host.publisher_key, 1, sizeof(g_host.publisher_key), input);
    trailing = fgetc(input);
    read_failed = ferror(input) || trailing != EOF;
    if (fclose(input) != 0)
        read_failed = 1;
    if (read_failed || g_host.publisher_key_size == 0)
        return 0;
    g_host.publisher_key_entry.spki = g_host.publisher_key;
    g_host.publisher_key_entry.spki_size = g_host.publisher_key_size;
    memset(&g_host.publisher_trust, 0, sizeof(g_host.publisher_trust));
    g_host.publisher_trust.struct_size = sizeof(g_host.publisher_trust);
    g_host.publisher_trust.keys = &g_host.publisher_key_entry;
    g_host.publisher_trust.key_count = 1;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.storage_root = g_host.storage_root;
    config.trust = g_host.publisher_trust;
    config.flags = PXA_POSIX_INSTALLER_FLAG_CLAIM_EMPTY_UNOWNED_DATA;
    pxa_package_limits_init(&config.limits);
    config.limits.max_components = 8;
    config.limits.max_permissions = PXA_C_HOST_MAX_APP_PERMISSIONS;
    g_host.package_limits = config.limits;
    installer_size = pxa_posix_installer_workspace_size(&config);
    g_host.installer_workspace = malloc(installer_size);
    return g_host.installer_workspace != NULL &&
           pxa_posix_installer_init(g_host.installer_workspace, installer_size, &config,
                                    &g_host.installer) == PXA_STATUS_OK;
}

static bool c_host_initialization_failed(void) {
    simulator_pxa_shutdown();
    return false;
}

bool simulator_pxa_initialize(void) {
    const char *ui_profile;
    if (g_host.initialized)
        return true;
    memset(&g_host, 0, sizeof(g_host));
    memcpy(g_host.locale, "en-US", sizeof("en-US"));
    g_host.locale_size = (uint8_t)(sizeof("en-US") - 1u);
    g_host.text_direction = 0;
    ui_profile = getenv("PXA_UI_PROFILE");
    g_host.ui_stress_profile =
        ui_profile != NULL && strcmp(ui_profile, "stress") == 0;
    g_host.active_component = PXA_COMPONENT_INVALID;
    g_host.fake_now_us = 1000000;
    snprintf(g_host.builtins_root, sizeof(g_host.builtins_root), "%s",
             PXA_SIMULATOR_PACKAGE_BUILTINS);
    snprintf(g_host.inbox_root, sizeof(g_host.inbox_root), "%s", PXA_SIMULATOR_PACKAGE_INBOX);
    snprintf(g_host.storage_root, sizeof(g_host.storage_root), "%s", PXA_SIMULATOR_PACKAGE_STATE);
    if (!make_dirs(g_host.storage_root) || !make_dirs(g_host.inbox_root) ||
        !c_host_initialize_runtime() || !c_host_initialize_storage() || !c_host_initialize_fs() ||
        !c_host_initialize_core_services() || !c_host_initialize_permission() ||
        !c_host_initialize_scheduler() || !c_host_initialize_providers() ||
        !c_host_initialize_execution() || !c_host_initialize_installer()) {
        return c_host_initialization_failed();
    }
    c_host_scan_builtins();
    c_host_load_disabled_policy();
    if (!c_host_register_services())
        return c_host_initialization_failed();
    c_host_install_all();
    c_host_scan_installed();
    c_host_scan_inbox();

    app_pages_set_app_catalog_callbacks(c_host_catalog_cb, c_host_launch_cb, NULL);
    app_pages_set_app_manager_callbacks(c_host_manager_cb, c_host_action_cb, NULL);
    app_pages_set_package_install_callback(c_host_install_package_cb, NULL);
    app_pages_set_package_preview_callback(c_host_preview_package_cb, NULL);
    app_pages_set_app_permission_callbacks(c_host_permission_list_cb, c_host_permission_set_cb,
                                           NULL);
    app_pages_set_runtime_permission_response_callback(c_host_runtime_permission_response, NULL);
    app_pages_set_back_request_callback(c_host_back_request_cb, NULL);
    g_host.maintenance_last_tick_ms = lv_tick_get();
    g_host.maintenance_timer = lv_timer_create(c_host_maintenance, 16, NULL);
    if (g_host.maintenance_timer == NULL) {
        return c_host_initialization_failed();
    }
    g_host.initialized = 1;
    return true;
}

/* The activation profile must describe the registered service versions,
 * independently of the Core protocol version. Keep this immutable table in
 * ROM rather than constructing a second, stale version map on every launch. */
static const pxa_package_service_capability_t c_host_service_capabilities[] = {
#define HOST_SERVICE(id, name, feature_bits) \
    {id, {name##_SERVICE_MAJOR, name##_SERVICE_MINOR}, feature_bits}
    HOST_SERVICE(PXA_SERVICE_CORE, PXA_CORE, 0),
    HOST_SERVICE(PXA_WINDOW_SERVICE_ID, PXA_WINDOW, 0),
    HOST_SERVICE(PXA_UI_SERVICE_ID, PXA_UI,
        PXA_UI_FEATURE_CANVAS | PXA_UI_FEATURE_VIRTUAL_LIST |
        PXA_UI_FEATURE_RGB565_BITMAP | PXA_UI_FEATURE_CONTROLLER_INPUT |
        PXA_UI_FEATURE_CANVAS_STREAM_IO),
    HOST_SERVICE(PXA_CLOCK_SERVICE_ID, PXA_CLOCK, 0),
    HOST_SERVICE(PXA_FS_SERVICE_ID, PXA_FS, 0),
    HOST_SERVICE(PXA_STORAGE_SERVICE_ID, PXA_STORAGE, 0),
    HOST_SERVICE(PXA_IPC_SERVICE_ID, PXA_IPC, 0),
    HOST_SERVICE(PXA_SENSOR_SERVICE_ID, PXA_SENSOR, 0),
    HOST_SERVICE(PXA_NET_SERVICE_ID, PXA_NET, 0),
    HOST_SERVICE(PXA_AUDIO_SERVICE_ID, PXA_AUDIO, 0),
    HOST_SERVICE(PXA_PERMISSION_SERVICE_ID, PXA_PERMISSION, 0),
    HOST_SERVICE(PXA_WORK_SERVICE_ID, PXA_WORK, 0),
    HOST_SERVICE(PXA_WASI_SERVICE_ID, PXA_WASI,
        PXA_WASI_FEATURE_CLOCKS | PXA_WASI_FEATURE_RANDOM),
    HOST_SERVICE(PXA_DEVICE_SERVICE_ID, PXA_DEVICE, 0),
    HOST_SERVICE(PXA_SURFACE_SERVICE_ID, PXA_SURFACE, 0),
    /* This compatibility backend has no Assets.bind callback. */
    HOST_SERVICE(PXA_GAME_RENDER_SERVICE_ID, PXA_GAME_RENDER, 0),
#undef HOST_SERVICE
};

bool simulator_pxa_start(const char* app_id) {
    size_t index;
    size_t declared_job_count = 0;
    pxa_package_host_profile_t host_profile;
    pxa_package_activation_profile_t capabilities;
    pxa_activation_plan_t* plan = NULL;
    pxa_c_host_app_t* app = NULL;
    pxa_package_manifest_t* manifest = NULL;
    size_t plan_size;
    pxa_status_t status = PXA_STATUS_INVALID_ARGUMENT;
    const char* stage = "bind application data";
    if (!g_host.initialized || app_id == NULL)
        return false;
    for (index = 0; index < g_host.app_count; ++index) {
        if (strcmp(g_host.apps[index].id, app_id) == 0) {
            app = &g_host.apps[index];
            break;
        }
    }
    if (app == NULL || !app->installed || !app->enabled)
        return false;
    if (g_host.active_app != NULL || g_host.coordinator != NULL) {
        c_host_stop_active();
    }
    /* Services stay registered, while their adapter contexts are rebound to
     * this Package only after the previous Package is fully deactivated. */
    if (!c_host_bind_app_data(app)) {
        app_pages_post_toast("应用数据加载失败", 1800);
        goto launch_failed;
    }
    g_host.active_app = app;
    stage = "bind permissions";
    if (!c_host_rebind_permissions(app)) {
        app_pages_post_toast("权限策略加载失败", 1800);
        goto launch_failed;
    }
    stage = "check required permissions";
    if (!c_host_app_permissions_can_activate(app)) {
        app_pages_post_toast("应用所需权限未授权", 1800);
        goto launch_failed;
    }
    stage = "load manifest";
    if (!c_host_load_current_app(app, &g_host.result_manifest_buffers,
                                 &manifest, app->root, sizeof(app->root))) {
        goto launch_failed;
    }
    g_host.active_manifest = manifest;
    c_host_clear_ui_assets();
    for (index = 0; index < manifest->component_count; ++index) {
        if (manifest->components[index].kind == 3)
            ++declared_job_count;
    }
    stage = "check declaration budgets";
    if (manifest->permission_count > PXA_C_HOST_MAX_APP_PERMISSIONS ||
        manifest->permission_count != app->permission_count ||
        declared_job_count > PXA_C_HOST_MAX_JOBS ||
        declared_job_count != app->job_component_count) {
        app_pages_post_toast("应用声明超出宿主限制", 1800);
        goto launch_failed;
    }

    memset(&host_profile, 0, sizeof(host_profile));
    host_profile.target = (pxa_bytes_t){(const uint8_t*)"linux-x86_64",
                                        sizeof("linux-x86_64") - 1u};
    host_profile.engine = (pxa_bytes_t){(const uint8_t*)"wamr", 4};
    host_profile.engine_abi = (pxa_bytes_t){
        (const uint8_t*)PXSYS_WAMR_ENGINE_ABI,
        sizeof(PXSYS_WAMR_ENGINE_ABI) - 1u};
    host_profile.memory_model = PXA_MEMORY_WASM32;
    memset(&capabilities, 0, sizeof(capabilities));
    capabilities.core_version.major = PXA_CORE_VERSION_MAJOR;
    capabilities.core_version.minor = PXA_CORE_VERSION_MINOR;
    capabilities.services = c_host_service_capabilities;
    capabilities.service_count = sizeof(c_host_service_capabilities) /
                                 sizeof(c_host_service_capabilities[0]);
    stage = "prepare activation plan";
    plan_size = pxa_activation_plan_workspace_size(manifest->component_count);
    status = PXA_STATUS_RESOURCE_LIMIT;
    g_host.plan_workspace = malloc(plan_size);
    if (g_host.plan_workspace == NULL)
        goto launch_failed;
    status = pxa_activation_plan_prepare(
        g_host.plan_workspace, plan_size, manifest, &capabilities, &host_profile,
        (pxa_bytes_t){(const uint8_t*)app->root, strlen(app->root)}, &plan);
    if (status != PXA_STATUS_OK)
        goto launch_failed;
    {
        stage = "initialize activation coordinator";
        status = PXA_STATUS_RESOURCE_LIMIT;
        size_t coordinator_size = pxa_activation_coordinator_workspace_size(plan);
        g_host.coordinator_workspace = malloc(coordinator_size);
        if (g_host.coordinator_workspace == NULL)
            goto launch_failed;
        status = pxa_activation_coordinator_init(g_host.coordinator_workspace, coordinator_size,
                                                 g_host.runtime, plan, &g_host.engine_ops,
                                                 &g_host.coordinator);
        if (status != PXA_STATUS_OK)
            goto launch_failed;
    }
    g_host.next_instance_id = 1;
    c_host_clear_active_jobs();
    stage = "activate main component";
    status = pxa_activation_activate(g_host.coordinator, (pxa_bytes_t){(const uint8_t*)"main", 4},
                                     g_host.next_instance_id++, &g_host.active_component);
    if (status != PXA_STATUS_OK)
        goto launch_failed;
    stage = "track main component";
    if (!c_host_track_active_component(g_host.active_component)) {
        goto launch_failed;
    }
    stage = "register IPC endpoints";
    for (index = 0; index < manifest->ipc_endpoint_count; ++index) {
        const pxa_package_ipc_endpoint_t* endpoint = &manifest->ipc_endpoints[index];
        pxa_component_t endpoint_component = PXA_COMPONENT_INVALID;
        uint64_t endpoint_instance = 0;
        status = pxa_activation_find(g_host.coordinator, endpoint->component_id, &endpoint_instance,
                                     &endpoint_component);
        if (status == PXA_STATUS_NOT_FOUND) {
            status = pxa_activation_activate(g_host.coordinator, endpoint->component_id,
                                             g_host.next_instance_id++, &endpoint_component);
        }
        if (status != PXA_STATUS_OK ||
            pxa_ipc_endpoint_register(g_host.ipc_broker, endpoint->name, endpoint_component) !=
                PXA_STATUS_OK ||
            !c_host_track_active_component(endpoint_component)) {
            goto launch_failed;
        }
    }
    c_host_drain_active_events();
    app_pages_dismiss_app_launch();
    /* Event delivery happens on the maintenance timer, not inside the UI
     * callback that triggered the launch. */
    return true;

launch_failed:
    fprintf(stderr, "PXA launch failed: app=%s stage=%s status=%d\n",
            app_id, stage, (int)status);
    c_host_stop_active();
    app_pages_dismiss_app_launch();
    return false;
}

void simulator_pxa_stop(void) {
    if (g_host.initialized)
        c_host_stop_active();
}

bool simulator_pxa_is_active(void) {
    return g_host.initialized &&
           g_host.active_component != PXA_COMPONENT_INVALID;
}

bool simulator_pxa_post_system_event(uint16_t opcode, const void *payload,
                                     size_t payload_size) {
    if (!g_host.initialized || g_host.runtime == NULL ||
        g_host.active_component == PXA_COMPONENT_INVALID ||
        opcode < UINT16_C(0x8000) ||
        (payload == NULL && payload_size != 0)) {
        return false;
    }
    return pxa_event_post_message(
               g_host.runtime, g_host.active_component, UINT16_C(17), opcode,
               0, (pxa_bytes_t){(const uint8_t *)payload, payload_size}, 1,
               0) == PXA_STATUS_OK;
}

bool simulator_pxa_post_key(uint32_t key) {
    const int32_t value = (int32_t)key;
    if (!g_host.initialized || g_host.active_app == NULL ||
        g_host.ui_service == NULL ||
        g_host.ui_component == PXA_COMPONENT_INVALID) {
        return false;
    }
    if (pxa_ui_queue_event(g_host.ui_service, g_host.ui_component,
                           PXA_UI_PRIMARY_SURFACE, 1u, PXA_UI_EVENT_KEY,
                           PXA_UI_EVENT_FLAG_RELIABLE, host_now_us(NULL), &value,
                           sizeof(value)) != PXA_STATUS_OK) {
        return false;
    }

    /* Keyboard volume events need the same immediate feel as a hardware key.
     * The regular maintenance tick remains responsible for all other events. */
    c_host_drain_active_events();
    return true;
}

bool simulator_pxa_post_controller_state(uint8_t controller, bool connected,
                                         uint32_t buttons) {
    uint8_t value[8] = {0};
    if (!connected) buttons = 0;
    if ((buttons & ~PXA_UI_CONTROLLER_BUTTON_MASK) != 0 ||
        !g_host.initialized || g_host.active_app == NULL ||
        g_host.ui_service == NULL ||
        g_host.ui_component == PXA_COMPONENT_INVALID) {
        return false;
    }

    value[0] = controller;
    value[1] = connected ? 1u : 0u;
    pxa_write_u32(value + 4, buttons);
    if (pxa_ui_queue_event(g_host.ui_service, g_host.ui_component,
                           PXA_UI_PRIMARY_SURFACE, 1u,
                           PXA_UI_EVENT_CONTROLLER_STATE,
                           PXA_UI_EVENT_FLAG_COALESCIBLE, host_now_us(NULL),
                           value, sizeof(value)) != PXA_STATUS_OK) {
        return false;
    }

    c_host_drain_active_events();
    return true;
}

void simulator_pxa_pointer_probe_reset(void) {
    g_host.pointer_probe_count = 0;
}

uint32_t simulator_pxa_pointer_probe_count(void) {
    return g_host.pointer_probe_count;
}

void simulator_pxa_ui_event_probe_reset(void) {
    g_host.ui_event_probe_count = 0;
}

uint32_t simulator_pxa_ui_event_probe_count(void) {
    return g_host.ui_event_probe_count;
}

static int c_host_open_lab_module(uint8_t module_index);

bool simulator_pxa_ipc_self_test(void) {
    uint8_t event[PXA_MAX_CONTROL_MESSAGE];
    pxa_event_view_t view;
    pxa_message_view_t message;
    size_t event_size = 0;
    bool passed = false;
    if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(3))
        return false;

    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, 12,
                       PXA_UI_EVENT_ACTION,
                       PXA_UI_EVENT_FLAG_RELIABLE, NULL, 0, NULL);
    if (!c_host_deliver_active_event_round() || !c_host_deliver_active_event_round() ||
        pxa_event_peek(g_host.runtime, g_host.active_component, &view) != PXA_STATUS_OK ||
        view.size > sizeof(event) ||
        pxa_event_pop(g_host.runtime, g_host.active_component, event, sizeof(event), &event_size) !=
            PXA_STATUS_OK ||
        pxa_message_decode(event, event_size, sizeof(event), &message) != PXA_STATUS_OK) {
        goto done;
    }
    passed = message.service == PXA_IPC_SERVICE_ID && message.opcode == PXA_IPC_REPLY_EVENT &&
             message.request_token != 0 && message.payload.size >= 4 &&
             (pxa_status_t)pxa_read_u32(message.payload.data) == PXA_STATUS_OK;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_state_journal_self_test(void) {
    pxa_c_host_app_t *app = NULL;
    char data_root[PXA_C_HOST_MAX_PATH];
    char journal_path[PXA_C_HOST_MAX_PATH];
    struct stat metadata;
    uint64_t before = 0;
    uint64_t after = 0;
    bool passed = false;

    for (size_t index = 0; index < g_host.app_count; ++index) {
        if (strcmp(g_host.apps[index].id, "pxa-lab") == 0) {
            app = &g_host.apps[index];
            break;
        }
    }
    if (app == NULL || !c_host_app_data_path(data_root, sizeof(data_root), app) ||
        snprintf(journal_path, sizeof(journal_path), "%s/journal/starts.log",
                 data_root) >= (int)sizeof(journal_path)) {
        return false;
    }
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        if (stat(journal_path, &metadata) == 0) {
            if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
                goto done;
            }
            before = (uint64_t)metadata.st_size;
        } else if (errno != ENOENT) {
            goto done;
        }
        if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(2)) {
            goto done;
        }
        if (stat(journal_path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            metadata.st_size < 0) {
            goto done;
        }
        after = (uint64_t)metadata.st_size;
        if (after != before + 1u) {
            goto done;
        }
        c_host_stop_active();
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_garden_freeze_test(void) {
    /* Headless reproduction of the garden-guard "开始冒险" freeze:
     * home -> tap start -> seed select -> tap fight -> run game ticks. */
    static const uint32_t game_node = 2u;
    uint8_t down[12];
    uint8_t up[12];
    int step;
    fprintf(stderr, "[garden-test] start\n");
    if (!simulator_pxa_start("pxa-garden-guard")) {
        fprintf(stderr, "[garden-test] FAILED: start\n");
        return false;
    }
    c_host_drain_active_events();
    fprintf(stderr, "[garden-test] initial render drained\n");

    memset(down, 0, sizeof(down));
    memset(up, 0, sizeof(up));
    down[1] = 0; /* PXA_POINTER_DOWN */
    up[1] = 2;   /* PXA_POINTER_UP */

    /* Inert tap first (5,60): exercises shared pointer path + home re-render
     * without hitting any button (fallback zone starts at y>=90). */
    down[4] = (uint8_t)5; down[8] = (uint8_t)60;
    up[4] = (uint8_t)5;   up[8] = (uint8_t)60;
    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, game_node,
                       PXA_UI_EVENT_POINTER, PXA_UI_EVENT_FLAG_RELIABLE,
                       down, sizeof(down), NULL);
    fprintf(stderr, "[garden-test] queued inert tap\n");
    c_host_drain_active_events();
    fprintf(stderr, "[garden-test] survived inert tap\n");

    /* Tap 开始冒险 center (148,164). */
    down[4] = (uint8_t)148; down[8] = (uint8_t)164;
    up[4] = (uint8_t)148;   up[8] = (uint8_t)164;
    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, game_node,
                       PXA_UI_EVENT_POINTER, PXA_UI_EVENT_FLAG_RELIABLE,
                       down, sizeof(down), NULL);
    fprintf(stderr, "[garden-test] queued start-adventure DOWN\n");
    c_host_drain_active_events();
    fprintf(stderr, "[garden-test] survived seed-select render\n");
    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, game_node,
                       PXA_UI_EVENT_POINTER, PXA_UI_EVENT_FLAG_RELIABLE,
                       up, sizeof(up), NULL);
    c_host_drain_active_events();
    for (step = 0; step < 6; ++step) {
        g_host.fake_now_us += 20000;
        c_host_post_clock_tick();
        c_host_drain_active_events();
    }
    fprintf(stderr, "[garden-test] seed-select idle ticks ok\n");

    /* Tap 开始战斗 (148,222). */
    down[4] = (uint8_t)148; down[8] = (uint8_t)222;
    up[4] = (uint8_t)148;   up[8] = (uint8_t)222;
    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, game_node,
                       PXA_UI_EVENT_POINTER, PXA_UI_EVENT_FLAG_RELIABLE,
                       down, sizeof(down), NULL);
    fprintf(stderr, "[garden-test] queued fight-start DOWN\n");
    c_host_drain_active_events();
    fprintf(stderr, "[garden-test] survived game reset+render\n");
    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, game_node,
                       PXA_UI_EVENT_POINTER, PXA_UI_EVENT_FLAG_RELIABLE,
                       up, sizeof(up), NULL);
    c_host_drain_active_events();

    for (step = 0; step < 120; ++step) {
        g_host.fake_now_us += 20000;
        c_host_post_clock_tick();
        c_host_drain_active_events();
    }
    fprintf(stderr, "[garden-test] 120 game ticks ok\n");
    c_host_stop_active();
    fprintf(stderr, "[garden-test] PASSED\n");
    return true;
}

static int c_host_drain_lab_test_events(void) {
    uint16_t round;
    for (round = 0; round < 128; ++round) {
        uint8_t index;
        int delivered = 0;
        for (index = 0; index < g_host.active_component_count; ++index) {
            const pxa_status_t status = pxa_wamr_engine_deliver_event(
                g_host.engine, g_host.runtime,
                g_host.active_components[index]);
            if (status == PXA_STATUS_OK) {
                delivered = 1;
            } else if (status != PXA_STATUS_WOULD_BLOCK) {
                fprintf(stderr,
                        "[lab-test] event delivery failed: round=%u "
                        "component=%u status=%d\n",
                        (unsigned)round,
                        (unsigned)g_host.active_components[index],
                        (int)status);
                return 0;
            }
        }
        if (!delivered) return 1;
    }
    fprintf(stderr, "[lab-test] event drain did not quiesce\n");
    return 0;
}

static uint32_t c_host_alpha_plane_checksum(void) {
    pxa_lvgl_ui_alpha_plane_t plane;
    uint32_t hash = UINT32_C(2166136261);
    uint32_t y;
    if (!pxa_lvgl_ui_alpha_plane(g_host.ui_adapter, &plane)) return 0;
    for (y = 0; y < plane.height; ++y) {
        const uint16_t *pixels = (const uint16_t *)(
            (const uint8_t *)plane.pixels + (size_t)y *
                                              plane.pixel_stride_bytes);
        const uint8_t *alpha = plane.alpha +
                               (size_t)y * plane.alpha_stride_bytes;
        uint32_t x;
        for (x = 0; x < plane.width; ++x) {
            hash = (hash ^ pixels[x]) * UINT32_C(16777619);
            hash = (hash ^ alpha[x]) * UINT32_C(16777619);
        }
    }
    return hash;
}

static int c_host_post_lab_test_event(uint32_t node,
                                      pxa_ui_event_kind_t kind,
                                      const void *value,
                                      size_t value_size) {
    const pxa_status_t status = pxa_ui_queue_event(
        g_host.ui_service, g_host.ui_component, PXA_UI_PRIMARY_SURFACE,
        node, kind, PXA_UI_EVENT_FLAG_RELIABLE, host_now_us(NULL), value,
        value_size);
    if (status != PXA_STATUS_OK) {
        fprintf(stderr,
                "[lab-test] UI event rejected: node=%u kind=%u status=%d\n",
                (unsigned)node, (unsigned)kind, (int)status);
        return 0;
    }
    return c_host_drain_lab_test_events();
}

static int c_host_post_pointer_phase(uint32_t node, uint16_t x, uint16_t y,
                                    uint8_t phase) {
    uint8_t value[12] = {0};
    value[1] = phase;
    pxa_write_u32(value + 4, x);
    pxa_write_u32(value + 8, y);
    return c_host_post_lab_test_event(node, PXA_UI_EVENT_POINTER,
                                      value, sizeof(value));
}

static int c_host_post_pointer_tap(uint32_t node, uint16_t x, uint16_t y) {
    return c_host_post_pointer_phase(node, x, y, 0u) &&
           c_host_post_pointer_phase(node, x, y, 2u);
}

static int c_host_post_pointer_drag(uint32_t node, uint16_t x,
                                    uint16_t from_y, uint16_t to_y) {
    const uint16_t middle_y = (uint16_t)((from_y + to_y) / 2u);
    return c_host_post_pointer_phase(node, x, from_y, 0u) &&
           c_host_post_pointer_phase(node, x, middle_y, 1u) &&
           c_host_post_pointer_phase(node, x, to_y, 1u) &&
           c_host_post_pointer_phase(node, x, to_y, 2u);
}

bool simulator_pxa_plane_shooter_self_test(void) {
    bool passed = false;
    if (!simulator_pxa_start("pxa-plane-shooter")) {
        for (size_t index = 0; index < g_host.app_count; ++index) {
            if (strcmp(g_host.apps[index].id, "pxa-plane-shooter") == 0) {
                fprintf(stderr,
                        "[plane-test] package installed=%u enabled=%u root=%s\n",
                        (unsigned)g_host.apps[index].installed,
                        (unsigned)g_host.apps[index].enabled,
                        g_host.apps[index].root);
            }
        }
        fprintf(stderr, "[plane-test] app start failed\n");
        goto done;
    }
    if (!c_host_drain_lab_test_events()) {
        fprintf(stderr, "[plane-test] initial event drain failed\n");
        goto done;
    }

    /* Visit each bottom section, return home, then launch the mission. */
    if (!c_host_post_pointer_tap(2u, 120u, 216u)) {
        fprintf(stderr, "[plane-test] hangar tap failed\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 178u, 216u) ||
        !c_host_post_pointer_drag(2u, 148u, 165u, 86u) ||
        !c_host_post_pointer_tap(2u, 238u, 216u)) {
        fprintf(stderr, "[plane-test] shop/mission navigation failed\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 230u, 84u)) {
        fprintf(stderr, "[plane-test] mission selection failed\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 54u, 216u)) {
        fprintf(stderr, "[plane-test] home tap failed\n"); goto done;
    }
    if (simulator_pxa_post_key(PXA_UI_KEY_VOLUME_UP)) {
        fprintf(stderr, "[plane-test] shell captured volume key\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 148u, 169u)) {
        fprintf(stderr, "[plane-test] launch tap failed\n"); goto done;
    }
    if (!simulator_pxa_post_key(PXA_UI_KEY_VOLUME_UP) ||
        !simulator_pxa_post_key(PXA_UI_KEY_VOLUME_UP_RELEASED)) {
        fprintf(stderr, "[plane-test] battle key capture missing\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 276u, 51u) ||
        !c_host_post_pointer_tap(2u, 148u, 101u)) {
        fprintf(stderr, "[plane-test] pause/resume failed\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 276u, 51u) ||
        !c_host_post_pointer_tap(2u, 148u, 139u)) {
        fprintf(stderr, "[plane-test] pause/restart failed\n"); goto done;
    }
    if (!c_host_post_pointer_tap(2u, 276u, 51u) ||
        !c_host_post_pointer_tap(2u, 148u, 177u)) {
        fprintf(stderr, "[plane-test] pause/exit failed\n"); goto done;
    }
    if (simulator_pxa_post_key(PXA_UI_KEY_VOLUME_UP)) {
        fprintf(stderr, "[plane-test] shell kept battle key capture\n"); goto done;
    }
    if (!c_host_drain_lab_test_events()) {
        fprintf(stderr, "[plane-test] battle event drain failed\n"); goto done;
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

static lv_obj_t *c_host_ui_find_label(lv_obj_t *object,
                                      const char *expected);
static lv_obj_t *c_host_ui_find_label_prefix(lv_obj_t *object,
                                             const char *prefix);

static lv_obj_t *c_host_menu_list(const char *title) {
    lv_obj_t *label = c_host_ui_find_label(lv_screen_active(), title);
    lv_obj_t *header;
    lv_obj_t *root;
    if (label == NULL) return NULL;
    header = lv_obj_get_parent(label);
    root = header == NULL ? NULL : lv_obj_get_parent(header);
    return root == NULL ? NULL : lv_obj_get_child(root, 1);
}

static int c_host_open_lab_module(uint8_t module_index) {
    if (module_index >= PXA_C_HOST_LAB_MODULE_COUNT) return 0;
    return c_host_drain_lab_test_events() &&
           c_host_post_lab_test_event(20u + module_index,
                                      PXA_UI_EVENT_ACTION, NULL, 0);
}

static int c_host_open_arcade_game(uint8_t game_index) {
    if (game_index >= PXA_C_HOST_ARCADE_GAME_COUNT) return 0;
    return c_host_drain_lab_test_events() &&
           c_host_post_lab_test_event(20u + game_index,
                                      PXA_UI_EVENT_ACTION, NULL, 0);
}

static void c_host_pump_ui(uint8_t iterations) {
    for (uint8_t iteration = 0; iteration < iterations; ++iteration) {
        SDL_Delay(10u);
        (void)lv_timer_handler();
    }
}

static int c_host_surface_frame_has_content(const c_host_surface_t *surface) {
    const uint16_t *pixels;
    size_t count;
    size_t index;
    uint16_t first;
    if (surface == NULL || surface->display_buffer == NULL ||
        surface->display_frame_bytes < sizeof(uint16_t)) {
        return 0;
    }
    pixels = (const uint16_t *)surface->display_buffer;
    count = surface->display_frame_bytes / sizeof(*pixels);
    first = pixels[0];
    for (index = 1; index < count; ++index) {
        if (pixels[index] != first) return 1;
    }
    return 0;
}

bool simulator_pxa_voxel_permission_self_test(void) {
    static const uint8_t audio_permission_name[] = "audio.playback";
    pxa_c_host_app_t *app = c_host_find_app("pxa-voxel-craft");
    c_host_app_permission_t *audio_permission = NULL;
    lv_obj_t *allow_label = NULL;
    uint8_t original_audio_grant = 0;
    uint8_t original_enabled = 0;
    uint32_t permission_prompt_id = 0;
    size_t permission_index;
    bool restored = true;
    bool passed = false;

    if (app == NULL || !app->installed) return false;
    for (permission_index = 0; permission_index < app->permission_count;
         ++permission_index) {
        c_host_app_permission_t *candidate = &app->permissions[permission_index];
        const pxa_permission_declaration_t *declaration;
        if (candidate->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[candidate->declaration_index];
        if (declaration->name.size == sizeof(audio_permission_name) - 1u &&
            memcmp(declaration->name.data, audio_permission_name,
                   sizeof(audio_permission_name) - 1u) == 0) {
            audio_permission = candidate;
            break;
        }
    }
    if (audio_permission == NULL) return false;
    original_audio_grant = audio_permission->granted;
    original_enabled = app->enabled;
    audio_permission->granted = 0;
    app->enabled = 1;

    if (!simulator_pxa_start("pxa-voxel-craft")) {
        fprintf(stderr, "[voxel-permission-test] launch failed\n");
        goto done;
    }
    c_host_pump_ui(4u);
    permission_prompt_id = g_host.permission_prompt.prompt_id;
    allow_label = c_host_ui_find_label(lv_layer_top(), "允许");
    if (allow_label == NULL)
        allow_label = c_host_ui_find_label(lv_layer_top(), "Allow");
    if (!g_host.permission_prompt.active || permission_prompt_id == 0 ||
        allow_label == NULL || g_host.surface == NULL) {
        fprintf(stderr,
                "[voxel-permission-test] prompt and early Surface did not "
                "coexist: active=%u prompt=%u allow=%u surface=%u\n",
                g_host.permission_prompt.active ? 1u : 0u,
                (unsigned)permission_prompt_id,
                allow_label != NULL ? 1u : 0u,
                g_host.surface != NULL ? 1u : 0u);
        goto done;
    }
    /* Complete Surface creation while the permission modal is still active.
     * The game may pre-render, but trusted UI must remain above that frame. */
    c_host_drain_active_events();
    c_host_pump_ui(4u);
    allow_label = c_host_ui_find_label(lv_layer_top(), "允许");
    if (allow_label == NULL)
        allow_label = c_host_ui_find_label(lv_layer_top(), "Allow");
    if (!g_host.permission_prompt.active || allow_label == NULL ||
        !g_host.surface->mapped_registered ||
        g_host.surface->buffer_count != 3u || g_host.surface->scale != 2u ||
        !g_host.surface->has_display_frame ||
        g_host.surface->presented_frames == 0 ||
        !c_host_surface_frame_has_content(g_host.surface)) {
        fprintf(stderr,
                "[voxel-permission-test] mapped first frame displaced trusted "
                "UI: active=%u allow=%u mapped=%u buffers=%u scale=%u "
                "frame=%u presented=%u content=%u\n",
                g_host.permission_prompt.active ? 1u : 0u,
                allow_label != NULL ? 1u : 0u,
                g_host.surface->mapped_registered ? 1u : 0u,
                g_host.surface->buffer_count, g_host.surface->scale,
                g_host.surface->has_display_frame ? 1u : 0u,
                (unsigned)g_host.surface->presented_frames,
                c_host_surface_frame_has_content(g_host.surface) ? 1u : 0u);
        goto done;
    }
    (void)lv_obj_send_event(lv_obj_get_parent(allow_label), LV_EVENT_CLICKED,
                            NULL);
    if (g_host.permission_prompt.active) {
        fprintf(stderr,
                "[voxel-permission-test] permission response failed\n");
        goto done;
    }
    c_host_drain_active_events();
    c_host_pump_ui(4u);
    if (g_host.permission_prompt.active || g_host.surface == NULL ||
        !g_host.surface->mapped_registered ||
        g_host.surface->buffer_count != 3u || g_host.surface->scale != 2u ||
        !g_host.surface->has_display_frame ||
        g_host.surface->presented_frames == 0) {
        fprintf(stderr,
                "[voxel-permission-test] mapped Surface did not survive "
                "permission grant: active=%u surface=%u mapped=%u "
                "buffers=%u scale=%u frame=%u presented=%u\n",
                g_host.permission_prompt.active ? 1u : 0u,
                g_host.surface != NULL ? 1u : 0u,
                g_host.surface != NULL && g_host.surface->mapped_registered
                    ? 1u : 0u,
                g_host.surface != NULL ? g_host.surface->buffer_count : 0u,
                g_host.surface != NULL ? g_host.surface->scale : 0u,
                g_host.surface != NULL && g_host.surface->has_display_frame
                    ? 1u : 0u,
                g_host.surface != NULL
                    ? (unsigned)g_host.surface->presented_frames : 0u);
        goto done;
    }
    passed = true;

done:
    if (g_host.permission_prompt.active) {
        c_host_dismiss_permission_prompt();
    } else if (permission_prompt_id != 0) {
        app_pages_dismiss_runtime_permission_prompt(permission_prompt_id);
    }
    c_host_stop_active();
    audio_permission->granted = original_audio_grant;
    app->enabled = original_enabled;
    restored = c_host_save_app_permission_policy(app) == PXA_STATUS_OK;
    if (!restored) {
        fprintf(stderr,
                "[voxel-permission-test] permission policy restore failed\n");
    }
    return passed && restored;
}

bool simulator_pxa_arcade_self_test(void) {
    uint8_t game_index;
    bool passed = false;

    for (game_index = 0; game_index < PXA_C_HOST_ARCADE_GAME_COUNT;
         ++game_index) {
        if (!simulator_pxa_start("pxa-arcade") ||
            !c_host_open_arcade_game(game_index) ||
            !simulator_pxa_is_active() ||
            !c_host_back_request_cb(NULL) ||
            !simulator_pxa_is_active() ||
            !c_host_open_arcade_game(game_index) ||
            !simulator_pxa_is_active() ||
            !c_host_drain_lab_test_events()) {
            fprintf(stderr, "[arcade-test] failed to reopen game %u\n",
                    (unsigned)game_index);
            goto done;
        }
        c_host_stop_active();
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_lab_self_test(void) {
    static const uint8_t audio_permission_name[] = "audio.playback";
    pxa_c_host_app_t *app = c_host_find_app("pxa-lab");
    c_host_app_permission_t *audio_permission = NULL;
    uint8_t original_audio_grant = 0;
    uint8_t original_enabled = 0;
    uint8_t module_index;
    size_t permission_index;
    bool passed = false;

    if (app == NULL || !app->installed) return false;
    for (permission_index = 0; permission_index < app->permission_count;
         ++permission_index) {
        c_host_app_permission_t *candidate = &app->permissions[permission_index];
        const pxa_permission_declaration_t *declaration;
        if (candidate->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[candidate->declaration_index];
        if (declaration->name.size == sizeof(audio_permission_name) - 1u &&
            memcmp(declaration->name.data, audio_permission_name,
                   sizeof(audio_permission_name) - 1u) == 0) {
            audio_permission = candidate;
            break;
        }
    }
    if (audio_permission == NULL) return false;
    original_audio_grant = audio_permission->granted;
    original_enabled = app->enabled;
    audio_permission->granted = 1;
    app->enabled = 1;

    for (module_index = 0; module_index < PXA_C_HOST_LAB_MODULE_COUNT;
         ++module_index) {
        if (!simulator_pxa_start("pxa-lab") ||
            !c_host_open_lab_module(module_index) ||
            !simulator_pxa_is_active() ||
            !c_host_drain_lab_test_events()) {
            fprintf(stderr, "[lab-test] failed to open module %u\n",
                    (unsigned)module_index);
            goto done;
        }
        if (module_index == 0u) {
            lv_obj_t *state_label;
            if (!c_host_post_lab_test_event(12u, PXA_UI_EVENT_ACTION, NULL,
                                            0)) {
                fprintf(stderr, "[lab-test] audio playback did not start\n");
                goto done;
            }
            state_label = c_host_ui_find_label_prefix(lv_screen_active(),
                                                       "submitted ");
            if (state_label == NULL ||
                !c_host_post_lab_test_event(12u, PXA_UI_EVENT_ACTION, NULL,
                                            0)) {
                fprintf(stderr, "[lab-test] audio query did not render\n");
                goto done;
            }
            state_label = c_host_ui_find_label_prefix(lv_screen_active(),
                                                       "submitted ");
            if (state_label == NULL ||
                strstr(lv_label_get_text(state_label), "queued 0") == NULL) {
                fprintf(stderr, "[lab-test] audio flush did not drain queue\n");
                goto done;
            }
        }
        if (module_index == 1u &&
            (!c_host_post_lab_test_event(12u, PXA_UI_EVENT_ACTION, NULL, 0) ||
             c_host_ui_find_label(lv_screen_active(), "完成。分数为每秒工作负载迭代次数") == NULL)) {
            fprintf(stderr, "[lab-test] benchmark did not complete\n");
            goto done;
        }
        if (module_index == 12u) {
            c_host_surface_t *surface = g_host.surface;
            uint8_t fragment_index;
            if (g_host.clock_period_ms != 16u || surface == NULL ||
                surface->fragment_count != 4u) {
                fprintf(stderr, "[lab-test] Surface overlay was not composed\n");
                goto done;
            }
            for (fragment_index = 0; fragment_index < surface->fragment_count;
                 ++fragment_index) {
                const c_host_surface_fragment_t *fragment =
                    &surface->fragments[fragment_index];
                const uint32_t right = (uint32_t)fragment->x + fragment->width;
                const uint32_t bottom =
                    (uint32_t)fragment->y + fragment->height;
                if (fragment->x < 132u && right > 8u &&
                    fragment->y < 42u && bottom > 8u) {
                    fprintf(stderr,
                            "[lab-test] Surface image covers LVGL overlay\n");
                    goto done;
                }
            }
            if (!c_host_post_lab_test_event(4u, PXA_UI_EVENT_ACTION, NULL, 0) ||
                c_host_ui_find_label(lv_screen_active(), "LVGL overlay 1") ==
                    NULL) {
                fprintf(stderr, "[lab-test] Surface LVGL overlay was not active\n");
                goto done;
            }
        }
        if (module_index == 13u) {
            c_host_surface_t *surface = g_host.surface;
            uint8_t step;
            lv_obj_t *metrics;
            lv_obj_t *overlay_button;
            lv_obj_t *overlay_label;
            pxa_lvgl_ui_alpha_plane_t alpha_plane;
            uint32_t normal_checksum;
            uint32_t pressed_checksum;
            uint32_t released_checksum;
            if (g_host.clock_period_ms != 33u || surface == NULL ||
                surface->fragment_count != 1u ||
                surface->ui_alpha_object == NULL ||
                surface->ui_alpha_image.header.w != 168u ||
                surface->ui_alpha_image.header.h != 40u ||
                surface->width != 208u || surface->height != 144u ||
                surface->frame_bytes != 208u * 144u * 4u ||
                surface->format != PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED ||
                surface->flags != PXA_SURFACE_FLAG_PREMULTIPLIED_ALPHA ||
                surface->image.header.cf !=
                    LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED ||
                c_host_ui_find_label(lv_screen_active(),
                                     "Premultiplied Alpha Surface") == NULL) {
                fprintf(stderr, "[lab-test] alpha Surface did not start\n");
                goto done;
            }
            if (!c_host_post_lab_test_event(4u, PXA_UI_EVENT_ACTION, NULL, 0) ||
                c_host_ui_find_label(lv_screen_active(), "LVGL overlay 1") ==
                    NULL) {
                fprintf(stderr, "[lab-test] alpha Surface overlay was not active\n");
                goto done;
            }
            overlay_label = c_host_ui_find_label(lv_screen_active(),
                                                 "LVGL overlay 1");
            overlay_button = overlay_label == NULL
                                 ? NULL : lv_obj_get_parent(overlay_label);
            if (overlay_label == NULL ||
                lv_obj_has_flag(overlay_button, LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_get_style_text_opa(overlay_label, 0) != LV_OPA_TRANSP ||
                lv_obj_get_style_bg_opa(overlay_button, 0) != LV_OPA_TRANSP ||
                !pxa_lvgl_ui_alpha_plane(g_host.ui_adapter, &alpha_plane) ||
                alpha_plane.alpha[0] != LV_OPA_TRANSP ||
                alpha_plane.alpha[alpha_plane.width - 1u] != LV_OPA_TRANSP ||
                alpha_plane.alpha[(size_t)(alpha_plane.height - 1u) *
                                      alpha_plane.alpha_stride_bytes] !=
                    LV_OPA_TRANSP ||
                alpha_plane.alpha[(size_t)(alpha_plane.height - 1u) *
                                      alpha_plane.alpha_stride_bytes +
                                  alpha_plane.width - 1u] != LV_OPA_TRANSP ||
                alpha_plane.alpha[(size_t)(alpha_plane.height / 2u) *
                                      alpha_plane.alpha_stride_bytes +
                                  alpha_plane.width / 2u] != LV_OPA_COVER) {
                fprintf(stderr,
                        "[lab-test] alpha Surface overlay shape was invalid\n");
                goto done;
            }
            normal_checksum = c_host_alpha_plane_checksum();
            lv_obj_add_state(overlay_button, LV_STATE_PRESSED);
            pressed_checksum = c_host_alpha_plane_checksum();
            if (lv_obj_get_style_transform_width(overlay_button,
                                                 LV_PART_MAIN) != 0 ||
                lv_obj_get_style_transform_height(overlay_button,
                                                  LV_PART_MAIN) != 0 ||
                normal_checksum == 0 || pressed_checksum == normal_checksum) {
                fprintf(stderr,
                        "[lab-test] alpha Surface pressed shape was unstable\n");
                goto done;
            }
            lv_obj_remove_state(overlay_button, LV_STATE_PRESSED);
            released_checksum = c_host_alpha_plane_checksum();
            if (released_checksum != normal_checksum) {
                fprintf(stderr,
                        "[lab-test] alpha Surface button state was not composited\n");
                goto done;
            }
            for (step = 0; step < 70u; ++step) {
                g_host.fake_now_us += 16000u;
                c_host_post_clock_tick();
                if (!c_host_drain_lab_test_events()) {
                    fprintf(stderr,
                            "[lab-test] alpha Surface animation failed at frame %u\n",
                            (unsigned)step);
                    goto done;
                }
            }
            metrics = c_host_ui_find_label_prefix(lv_screen_active(), "FPS ");
            if (metrics == NULL ||
                strncmp(lv_label_get_text(metrics), "FPS 30.", 7u) != 0) {
                fprintf(stderr,
                        "[lab-test] alpha Surface cadence was not 30fps: %s\n",
                        metrics == NULL ? "missing" : lv_label_get_text(metrics));
                goto done;
            }
        }
        if (module_index == 16u) {
            uint8_t step;
            if (g_host.clock_period_ms != 16u) {
                fprintf(stderr,
                        "[lab-test] RGB565 animation period was %u ms\n",
                        (unsigned)g_host.clock_period_ms);
                goto done;
            }
            for (step = 0; step < 40u; ++step) {
                g_host.fake_now_us += 16000u;
                c_host_post_clock_tick();
                if (!c_host_drain_lab_test_events()) {
                    fprintf(stderr,
                            "[lab-test] RGB565 animation failed at frame %u\n",
                            (unsigned)step);
                    goto done;
                }
            }
            if (!c_host_post_pointer_tap(2u, 148u, 120u) ||
                g_host.clock_period_ms != 16u) {
                fprintf(stderr, "[lab-test] RGB565 mode switch did not complete\n");
                goto done;
            }
        }
        if (module_index == 17u &&
            (!simulator_pxa_post_controller_state(
                 0, true, PXA_UI_CONTROLLER_RIGHT | PXA_UI_CONTROLLER_A) ||
             c_host_ui_find_label(lv_screen_active(), "P1: RIGHT A") == NULL ||
             !simulator_pxa_post_controller_state(0, false, 0) ||
             c_host_ui_find_label(lv_screen_active(),
                                  "Controller disconnected / waiting") ==
                 NULL)) {
            fprintf(stderr, "[lab-test] controller state did not render\n");
            goto done;
        }
        c_host_drain_lab_test_events();
        c_host_stop_active();
    }
    passed = true;

done:
    c_host_stop_active();
    audio_permission->granted = original_audio_grant;
    app->enabled = original_enabled;
    return passed;
}

bool simulator_pxa_fs_self_test(void) {
    static const uint32_t actions[] = {20u, 22u, 23u, 21u, 22u, 24u, 23u};
    size_t index;
    bool passed = false;

    if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(2u)) {
        fprintf(stderr, "[fs-test] failed to open Private Files module\n");
        goto done;
    }
    for (index = 0; index < sizeof(actions) / sizeof(actions[0]); ++index) {
        if (!c_host_post_lab_test_event(actions[index], PXA_UI_EVENT_ACTION,
                                        NULL, 0)) {
            fprintf(stderr, "[fs-test] action %u failed\n",
                    (unsigned)actions[index]);
            goto done;
        }
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_storage_self_test(void) {
    static const uint32_t actions[] = {20u, 21u, 22u, 23u};
    size_t index;
    bool passed = false;

    if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(10u)) {
        fprintf(stderr, "[storage-test] failed to open Storage module\n");
        goto done;
    }
    for (index = 0; index < sizeof(actions) / sizeof(actions[0]); ++index) {
        if (!c_host_post_lab_test_event(actions[index], PXA_UI_EVENT_ACTION,
                                        NULL, 0)) {
            fprintf(stderr, "[storage-test] action %u failed\n",
                    (unsigned)actions[index]);
            goto done;
        }
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_lease_self_test(void) {
    size_t index;
    bool passed = false;

    if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(5u) ||
        !c_host_post_lab_test_event(12u, PXA_UI_EVENT_ACTION, NULL, 0) ||
        !pxa_lease_has_active(g_host.lease_service)) {
        fprintf(stderr, "[lease-test] lease was not granted\n");
        goto done;
    }
    for (index = 0; index < 100u; ++index) c_host_maintenance(NULL);
    if (!pxa_lease_has_active(g_host.lease_service)) {
        fprintf(stderr, "[lease-test] lease expired before 1.6 seconds\n");
        goto done;
    }
    for (; index < 400u && pxa_lease_has_active(g_host.lease_service); ++index)
        c_host_maintenance(NULL);
    passed = !pxa_lease_has_active(g_host.lease_service) &&
             c_host_ui_find_label(lv_screen_active(), "租约已到期并被撤销") != NULL;
    if (!passed) fprintf(stderr, "[lease-test] lease did not expire as expected\n");

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_menu_scroll_self_test(void) {
    static const int32_t expected_scroll = 80;
    static const char *const app_ids[] = {"pxa-lab", "pxa-arcade"};
    static const char *const titles[] = {"PXA ABI 实验室", "PXA 游戏厅"};
    size_t index;
    bool passed = false;

    for (index = 0; index < sizeof(app_ids) / sizeof(app_ids[0]); ++index) {
        lv_obj_t *list;
        int32_t restored_scroll;
        if (!simulator_pxa_start(app_ids[index]) ||
            !c_host_post_lab_test_event(5u, PXA_UI_EVENT_SCROLL,
                                        &expected_scroll,
                                        sizeof(expected_scroll)) ||
            !(index == 0 ? c_host_open_lab_module(10u) :
                           c_host_open_arcade_game(6u)) ||
            !c_host_back_request_cb(NULL)) {
            fprintf(stderr, "[menu-scroll-test] failed to navigate %s\n",
                    app_ids[index]);
            goto done;
        }
        list = c_host_menu_list(titles[index]);
        restored_scroll = list == NULL ? 0 : lv_obj_get_scroll_y(list);
        if (restored_scroll < expected_scroll - 1 ||
            restored_scroll > expected_scroll + 1) {
            fprintf(stderr,
                    "[menu-scroll-test] %s restored %d, expected %d\n",
                    app_ids[index], (int)restored_scroll,
                    (int)expected_scroll);
            goto done;
        }
        c_host_stop_active();
    }
    passed = true;

done:
    c_host_stop_active();
    return passed;
}

bool simulator_pxa_work_self_test(void) {
    static const uint8_t key[] = "work.runs";
    uint8_t value = 0;
    size_t value_size = 0;
    size_t index;
    bool passed = false;
    const char *failure = "launch pxa-lab";

    if (!simulator_pxa_start("pxa-lab")) {
        fprintf(stderr, "[work-test] failed: %s\n", failure);
        return false;
    }
    failure = "reset Work queue";
    if (g_host.scheduler_store.save(g_host.scheduler_store.context, NULL, 0) != PXA_STATUS_OK ||
        pxa_scheduler_rebind(g_host.scheduler_service, g_host.active_app->job_components,
                             g_host.active_app->job_component_count) != PXA_STATUS_OK) {
        goto done;
    }
    (void)g_host.storage_backend.remove(g_host.storage_backend.context,
                                        (pxa_bytes_t){key, sizeof(key) - 1u});

    failure = "open Background Work module";
    if (!c_host_open_lab_module(11)) goto done;
    failure = "activate enqueue button";
    if (!c_host_post_lab_test_event(12, PXA_UI_EVENT_ACTION, NULL, 0))
        goto done;
    failure = "enqueue Work";
    if (!pxa_scheduler_has_pending(g_host.scheduler_service)) {
        goto done;
    }
    failure = "observe Work completion without leaving the module";
    for (index = 0; index < 180u; ++index) {
        c_host_maintenance(NULL);
        if (c_host_ui_find_label(lv_screen_active(), "后台任务已完成") != NULL)
            break;
    }
    if (index == 180u) goto done;
    failure = "return from Work module";
    if (!c_host_back_request_cb(NULL)) {
        goto done;
    }
    failure = "leave Lab while Work is queued";
    if (!c_host_back_request_cb(NULL)) {
        goto done;
    }
    for (index = 0; index < 126; ++index) {
        c_host_maintenance(NULL);
    }
    if (g_host.storage_backend.get(g_host.storage_backend.context,
                                   (pxa_bytes_t){key, sizeof(key) - 1u}, &value, sizeof(value),
                                   &value_size) != PXA_STATUS_OK) {
        failure = "read worker completion marker";
        goto done;
    }
    failure = "worker completion marker";
    passed = value_size == 1 && value == 1;
    if (passed) {
        c_host_stop_active();
        failure = "relaunch pxa-lab";
        passed = simulator_pxa_start("pxa-lab");
        if (passed) {
            if (!c_host_open_lab_module(11) ||
                !c_host_post_lab_test_event(12, PXA_UI_EVENT_ACTION,
                                            NULL, 0)) {
                passed = false;
                goto done;
            }
            failure = "persist queued Work across package restart";
            passed = pxa_scheduler_has_pending(g_host.scheduler_service) &&
                     simulator_pxa_start("pxa-lab") &&
                     pxa_scheduler_has_pending(g_host.scheduler_service);
        }
    }

done:
    if (!passed) fprintf(stderr, "[work-test] failed: %s\n", failure);
    if (g_host.active_app != NULL &&
        g_host.scheduler_store.save(g_host.scheduler_store.context, NULL, 0) == PXA_STATUS_OK) {
        (void)pxa_scheduler_rebind(g_host.scheduler_service,
                                   g_host.active_app->job_components,
                                   g_host.active_app->job_component_count);
    }
    c_host_stop_active();
    return passed;
}

static lv_obj_t *c_host_ui_find_label(lv_obj_t *object,
                                      const char *expected);

bool simulator_pxa_net_self_test(void) {
    static const uint8_t permission_name[] = "net.client";
    pxa_c_host_app_t *app = c_host_find_app("pxa-lab");
    c_host_app_permission_t *permission = NULL;
    uint8_t original_grant = 0;
    uint8_t original_enabled = 0;
    size_t index;
    bool passed = false;
    if (app == NULL || !app->installed) return false;
    for (index = 0; index < app->permission_count; ++index) {
        c_host_app_permission_t *candidate = &app->permissions[index];
        const pxa_permission_declaration_t *declaration;
        if (candidate->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[candidate->declaration_index];
        if (declaration->name.size == sizeof(permission_name) - 1u &&
            memcmp(declaration->name.data, permission_name,
                   sizeof(permission_name) - 1u) == 0) {
            permission = candidate;
            break;
        }
    }
    if (permission == NULL) return false;
    original_grant = permission->granted;
    original_enabled = app->enabled;
    permission->granted = 1;
    app->enabled = 1;
    if (!simulator_pxa_start("pxa-lab") || !c_host_open_lab_module(7))
        goto done;

    c_host_on_ui_event(PXA_UI_PRIMARY_SURFACE, 43,
                       PXA_UI_EVENT_ACTION,
                       PXA_UI_EVENT_FLAG_RELIABLE, NULL, 0, NULL);
    c_host_drain_active_events();
    for (index = 0; index < 625u; ++index) {
        c_host_maintenance(NULL);
        (void)lv_timer_handler();
        if (c_host_ui_find_label(lv_screen_active(), "PASS") != NULL) {
            passed = true;
            break;
        }
        usleep(16000);
    }

done:
    c_host_stop_active();
    permission->granted = original_grant;
    app->enabled = original_enabled;
    return passed;
}

static lv_obj_t *c_host_ui_find_label(lv_obj_t *object,
                                      const char *expected) {
    uint32_t index;
    if (object == NULL || expected == NULL) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) &&
        strcmp(lv_label_get_text(object), expected) == 0) return object;
    for (index = 0; index < lv_obj_get_child_count(object); ++index) {
        lv_obj_t *match = c_host_ui_find_label(
            lv_obj_get_child(object, (int32_t)index), expected);
        if (match != NULL) return match;
    }
    return NULL;
}

static lv_obj_t *c_host_ui_find_label_prefix(lv_obj_t *object,
                                             const char *prefix) {
    uint32_t index;
    size_t prefix_size;
    if (object == NULL || prefix == NULL) return NULL;
    prefix_size = strlen(prefix);
    if (lv_obj_check_type(object, &lv_label_class) &&
        strncmp(lv_label_get_text(object), prefix, prefix_size) == 0)
        return object;
    for (index = 0; index < lv_obj_get_child_count(object); ++index) {
        lv_obj_t *match = c_host_ui_find_label_prefix(
            lv_obj_get_child(object, (int32_t)index), prefix);
        if (match != NULL) return match;
    }
    return NULL;
}

bool simulator_pxa_weather_self_test(void) {
    static const uint8_t device_permission_name[] = "device.identity";
    static const uint8_t network_permission_name[] = "net.client";
    pxa_c_host_app_t *app = c_host_find_app("pxa-weather");
    c_host_app_permission_t *device_permission = NULL;
    c_host_app_permission_t *network_permission = NULL;
    uint8_t original_device_grant = 0;
    uint8_t original_network_grant = 0;
    uint8_t original_enabled = 0;
    size_t index;
    int refresh_visible = 0;
    lv_area_t refresh_area = {0};
    lv_area_t screen_area = {0};
    lv_obj_t *refresh_label;
    bool passed = false;
    if (app == NULL || !app->installed) {
        fprintf(stderr, "Weather diagnostics: app=%p installed=%d\n",
                (void *)app, app != NULL ? app->installed : 0);
        return false;
    }
    for (index = 0; index < app->permission_count; ++index) {
        c_host_app_permission_t *candidate = &app->permissions[index];
        const pxa_permission_declaration_t *declaration;
        if (candidate->declaration_index >= g_host.declaration_count) continue;
        declaration = &g_host.declarations[candidate->declaration_index];
        if (declaration->name.size == sizeof(device_permission_name) - 1u &&
            memcmp(declaration->name.data, device_permission_name,
                   sizeof(device_permission_name) - 1u) == 0) {
            device_permission = candidate;
        } else if (declaration->name.size ==
                       sizeof(network_permission_name) - 1u &&
                   memcmp(declaration->name.data, network_permission_name,
                          sizeof(network_permission_name) - 1u) == 0) {
            network_permission = candidate;
        }
    }
    if (device_permission == NULL || network_permission == NULL) {
        fprintf(stderr, "Weather diagnostics: required permission missing\n");
        return false;
    }
    original_device_grant = device_permission->granted;
    original_network_grant = network_permission->granted;
    original_enabled = app->enabled;
    device_permission->granted = 1;
    network_permission->granted = 1;
    app->enabled = 1;
    if (!simulator_pxa_start("pxa-weather")) {
        fprintf(stderr, "Weather diagnostics: app start failed\n");
        goto done;
    }
    for (index = 0; index < 938u; ++index) {
        c_host_maintenance(NULL);
        (void)lv_timer_handler();
        if (c_host_ui_find_label(lv_screen_active(), "数据已更新") != NULL)
            break;
        usleep(16000);
    }
    (void)lv_timer_handler();
    lv_obj_update_layout(lv_screen_active());
    refresh_label = c_host_ui_find_label(lv_screen_active(), "刷新");
    if (refresh_label != NULL && lv_obj_get_parent(refresh_label) != NULL) {
        lv_obj_get_coords(lv_obj_get_parent(refresh_label), &refresh_area);
        lv_obj_get_coords(lv_screen_active(), &screen_area);
        refresh_visible = refresh_area.x1 >= screen_area.x1 &&
                          refresh_area.y1 >= screen_area.y1 &&
                          refresh_area.x2 <= screen_area.x2 &&
                          refresh_area.y2 <= screen_area.y2;
    }
    passed = c_host_ui_find_label(lv_screen_active(), "数据已更新") != NULL &&
             refresh_visible;
    if (!passed) {
        fprintf(stderr,
                "Weather diagnostics: pending=%d app_active=%d children=%u "
                "refresh_visible=%d "
                "refresh=(%d,%d)-(%d,%d) screen=(%d,%d)-(%d,%d)\n",
                pxa_net_has_pending_requests(g_host.net_service),
                simulator_pxa_is_active(),
                (unsigned)lv_obj_get_child_count(lv_screen_active()),
                refresh_visible,
                (int)refresh_area.x1, (int)refresh_area.y1,
                (int)refresh_area.x2, (int)refresh_area.y2,
                (int)screen_area.x1, (int)screen_area.y1,
                (int)screen_area.x2, (int)screen_area.y2);
    }

done:
    c_host_stop_active();
    device_permission->granted = original_device_grant;
    network_permission->granted = original_network_grant;
    app->enabled = original_enabled;
    return passed;
}

bool simulator_pxa_provider_self_test(void) {
    uint8_t audio_frame[PXA_C_HOST_AUDIO_SAMPLE_RATE *
                        PXA_C_HOST_AUDIO_CHANNELS * 2u *
                        PXA_C_HOST_AUDIO_FRAME_MS / 1000u];
    size_t index;
    pxa_audio_format_t format;
    pxa_audio_graph_t graph;
    uint64_t sessions[PXA_C_HOST_AUDIO_MAX_SESSIONS];
    void* subscription = NULL;
    int32_t values[PXA_SENSOR_MAX_DIMENSIONS] = {0};
    bool passed = false;
    memset(sessions, 0, sizeof(sessions));
    memset(audio_frame, 0, sizeof(audio_frame));
    memset(g_host.audio_sessions, 0, sizeof(g_host.audio_sessions));
    memset(&graph, 0, sizeof(graph));
    for (index = 0; index < PXA_C_HOST_AUDIO_MAX_SESSIONS; ++index) {
        memset(&format, 0, sizeof(format));
        if (c_host_audio_open(&g_host, 0, &format, &sessions[index]) != PXA_STATUS_OK ||
            format.sample_rate != 16000 || format.channels != 1 ||
            (index != 0 && sessions[index] == sessions[index - 1u])) {
            goto done;
        }
    }
    {
        uint64_t overflow = 0;
        if (c_host_audio_open(&g_host, 0, &format, &overflow) != PXA_STATUS_RESOURCE_LIMIT) {
            goto done;
        }
    }
    if (c_host_audio_submit(&g_host, sessions[0], NULL, 0) != PXA_STATUS_BAD_STATE ||
        c_host_audio_commit(&g_host, sessions[0], &graph) != PXA_STATUS_OK ||
        c_host_audio_submit(&g_host, sessions[0], audio_frame,
                            sizeof(audio_frame)) != PXA_STATUS_OK) {
        goto done;
    }
    if (c_host_sensor_subscribe(NULL, 1, 1000, &subscription) != PXA_STATUS_OK ||
        c_host_sensor_read(NULL, subscription, 1, values) != PXA_STATUS_OK || values[0] != 25000 ||
        c_host_sensor_subscribe(NULL, 2, 1000, &subscription) != PXA_STATUS_OK ||
        c_host_sensor_read(NULL, subscription, 2, values) != PXA_STATUS_OK || values[0] != 420000) {
        goto done;
    }
    passed = true;

done:
    for (index = 0; index < PXA_C_HOST_AUDIO_MAX_SESSIONS; ++index) {
        if (sessions[index] != 0) {
            c_host_audio_close(&g_host, sessions[index]);
        }
    }
    return passed;
}

void simulator_pxa_shutdown(void) {
    size_t net_index;
    if (g_host.coordinator != NULL) {
        (void)pxa_activation_deactivate_all(g_host.coordinator, PXA_STOP_NORMAL);
    }
    for (net_index = 0; net_index < PXA_C_HOST_NET_MAX_OPERATIONS; ++net_index) {
        c_host_net_dispose_operation(&g_host, &g_host.net_operations[net_index]);
    }
    if (g_host.curl_multi != NULL) {
        curl_multi_cleanup(g_host.curl_multi);
        g_host.curl_multi = NULL;
    }
    if (g_host.curl_initialized) {
        curl_global_cleanup();
        g_host.curl_initialized = 0;
    }
    c_host_audio_release_device(&g_host);
    if (g_host.maintenance_timer != NULL) {
        lv_timer_delete(g_host.maintenance_timer);
        g_host.maintenance_timer = NULL;
    }
    if (g_host.engine != NULL)
        pxa_wamr_engine_deinit(g_host.engine);
    if (g_host.ui_adapter != NULL)
        pxa_lvgl_ui_deinit(g_host.ui_adapter);
    if (g_host.ui_service != NULL)
        pxa_ui_service_deinit(g_host.ui_service);
    if (g_host.installer != NULL)
        pxa_posix_installer_deinit(g_host.installer);
    if (g_host.posix_scheduler_store != NULL) {
        pxa_posix_scheduler_store_deinit(g_host.posix_scheduler_store);
    }
    if (g_host.posix_fs != NULL)
        pxa_posix_fs_deinit(g_host.posix_fs);
    if (g_host.posix_storage != NULL) {
        pxa_posix_storage_deinit(g_host.posix_storage);
    }
    if (g_host.runtime != NULL)
        pxa_runtime_deinit(g_host.runtime);
    free(g_host.coordinator_workspace);
    free(g_host.plan_workspace);
    c_host_manifest_buffers_release(&g_host.result_manifest_buffers);
    free(g_host.installer_workspace);
    free(g_host.engine_workspace);
    free(g_host.ui_adapter_workspace);
    c_host_clear_ui_assets();
    free(g_host.ui_workspace);
    free(g_host.game_render_workspace);
    free(g_host.surface_workspace);
    free(g_host.audio_workspace);
    free(g_host.net_workspace);
    free(g_host.device_workspace);
    free(g_host.sensor_workspace);
    free(g_host.scheduler_workspace);
    free(g_host.scheduler_store_workspace);
    free(g_host.permission_workspace);
    free(g_host.lease_workspace);
    free(g_host.ipc_broker_workspace);
    free(g_host.window_workspace);
    free(g_host.fs_service_state_workspace);
    free(g_host.fs_service_workspace);
    free(g_host.storage_service_workspace);
    free(g_host.storage_workspace);
    free(g_host.runtime_workspace);
    memset(&g_host, 0, sizeof(g_host));
    g_host.active_component = PXA_COMPONENT_INVALID;
}

/* Actual native backend regression, independent of old bundled Apps. */
bool simulator_pxa_game_render_self_test(void) {
    if (!g_host.runtime || g_host.surface) return false;
    uint64_t id = 0; uint32_t caps = 0;
    pxa_game_render_desc_t desc = {g_display_metrics.width, g_display_metrics.height,
        2, 0, PXA_GAME_RENDER_SCRATCH_DEPTH16, 1024};
    if (c_host_game_render_create(&g_host, &desc, &id, &caps)) return false;
    bool ok = false;
    uint8_t bytes[PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES +
        2 * PXA_RASTER_TEXTURED_QUAD_BYTES] = {0};
    pxa_write_u32(bytes, PXA_RASTER_DRAW_MAGIC);
    pxa_write_u16(bytes+4, PXA_RASTER_ABI_MAJOR);
    pxa_write_u16(bytes+6, PXA_RASTER_ABI_MINOR);
    pxa_write_u32(bytes+8, sizeof(bytes));
    pxa_write_u32(bytes+12, PXA_RASTER_CAP_TEXTURED_QUAD);
    pxa_write_u32(bytes+16, 3); pxa_write_u64(bytes+20, 1);
    uint8_t *record = bytes + PXA_RASTER_DRAW_HEADER_BYTES;
    record[0] = PXA_RASTER_RECORD_CLEAR_RGB565;
    pxa_write_u16(record+2, PXA_RASTER_CLEAR_BYTES);
    for (unsigned q=0; q<2; ++q) {
        record = bytes + PXA_RASTER_DRAW_HEADER_BYTES + PXA_RASTER_CLEAR_BYTES + q * PXA_RASTER_TEXTURED_QUAD_BYTES;
        record[0] = PXA_RASTER_RECORD_TEXTURED_QUAD;
        record[1] = PXA_RASTER_QUAD_SOLID_COLOR;
        pxa_write_u16(record+2, PXA_RASTER_TEXTURED_QUAD_BYTES);
        pxa_write_u16(record+6, q==0 ? 0x07e0 : 0xf800);
        const int16_t xy[8] = {0,0,128,0,128,128,0,128};
        for (unsigned v=0; v<4; ++v) {
            uint8_t *vertex = record + 8 + v * PXA_RASTER_VERTEX_BYTES;
            pxa_write_u16(vertex, xy[2*v]); pxa_write_u16(vertex+2, xy[2*v+1]);
            vertex[8] = 255; pxa_write_u16(vertex+10, q==0 ? 256 : 768);
        }
    }
    c_host_surface_t *surface = (c_host_surface_t *)(uintptr_t)id;
    if (c_host_surface_raster_submit(&g_host,id,bytes,sizeof(bytes))) goto done;
    /* A newer queued frame replaces the old one, with no unbounded list cache. */
    pxa_write_u64(bytes+20,2);
    if (c_host_surface_raster_submit(&g_host,id,bytes,sizeof(bytes))) goto done;
    if (!c_host_surface_process_pending()) goto done;
    pxa_raster_telemetry_t stats;
    if (c_host_surface_raster_query(&g_host,id,&stats)) goto done;
    ok = surface->raster_draw_capacity==1024 && surface->raster_palette==NULL &&
        surface->raster_depth!=NULL && surface->presented_frames==1 && stats.submitted_frames==2 &&
        stats.dropped_frames==1 && ((uint16_t*)surface->buffers[surface->current])[4*surface->width+4]==0x07e0;
    /* The farther red quad cannot overwrite the nearer green quad. */
done:
    c_host_surface_close(&g_host,id);
    for (uint8_t mode=PXA_GAME_RENDER_SCRATCH_NONE; ok && mode<=PXA_GAME_RENDER_SCRATCH_COVERAGE_2BIT; ++mode) {
        desc.scratch_mode=mode;
        if (c_host_game_render_create(&g_host,&desc,&id,&caps)) return false;
        surface=(c_host_surface_t*)(uintptr_t)id;
        ok = (surface->raster_depth != NULL) == (mode != PXA_GAME_RENDER_SCRATCH_NONE);
        c_host_surface_close(&g_host,id);
    }
    return ok;
}
