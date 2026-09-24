/* ESP32 runtime host on the C stack. Implements the public host entry points
 * (boot, launch, back, manage) entirely on libpxa: the C WAMR engine
 * adapter, the C services with ESP backends (LittleFS permission policy,
 * LittleFS private data via the posix adapters), esp_timer clock and
 * watchdog, and the FreeRTOS command queue with a dedicated runtime
 * thread. Replaces the legacy C++ runtime host. */

#include "pxa_esp_host.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_pthread.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "pxa/activation.h"
#include "pxa/audio.h"
#include "pxa/fs.h"
#include "pxa/surface.h"
#include "pxa/ipc.h"
#include "pxa/lease.h"
#include "pxa/lvgl/pxa_lvgl_ui.h"
#include "pxa/net.h"
#include "pxa/package.h"
#include "pxa/permission.h"
#include "pxa/runtime.h"
#include "pxa/scheduler.h"
#include "pxa/sensor.h"
#include "pxa/storage.h"
#include "pxa/ui.h"
#include "pxa/wamr/pxa_wamr_engine.h"
#include "pxa/wasi.h"
#include "pxa/window.h"
#include "pxa/wire.h"
#include "pxa_esp_package_store.h"
#include "pxa_esp_store_download.h"
#include "pxa_esp_permission_store.h"
#include "pxa_esp_services.h"
#include "pxa_esp_ui_shell.h"
#include "pxa_esp_audio.h"
#include "pxa/pxa_esp_surface.h"
#include "pxa_esp_net.h"
#include "pxa_esp_ui_assets.h"
#include "pxa_host_activation_arena.h"
#include "pxa_host_clock_slots.h"
#include "pxa_host_command.h"
#include "pxa_host_pointer_mailbox.h"

#define PXA_ESP_HOST_TAG "PxaHost"
#define PXA_ESP_HOST_MAX_APP_ID PXA_HOST_COMMAND_MAX_IDENTITY_BYTES
#define PXA_ESP_HOST_MAX_PATH 160
#define PXA_ESP_HOST_MAX_SERVICES 19
#define PXA_ESP_HOST_EVENT_DRAIN_ROUNDS 3
#define PXA_ESP_HOST_BACK_EVENT_DRAIN_LIMIT 32
#define PXA_ESP_HOST_CLOCK_PERIOD_US 5000
#define PXA_ESP_HOST_POINTER_MOVE_MIN_INTERVAL_US 16000
#define PXA_ESP_HOST_MAINTENANCE_PERIOD_US 1000000
#define PXA_ESP_HOST_LIFECYCLE_PERIOD_US 100000
#define PXA_ESP_HOST_WORK_CANCEL_GRACE_MS UINT64_C(500)
#define PXA_ESP_HOST_UI_DYNAMIC_BYTES (512 * 1024)
#define PXA_ESP_HOST_UI_TRANSACTION_BYTES (192 * 1024)
#define PXA_ESP_HOST_UI_CANVAS_BYTES (128 * 1024)
#define PXA_ESP_WAMR_ALLOC_ALIGNMENT 8u

#define PXA_CORE_SERVICE_ID UINT16_C(1)
#define PXA_CLOCK_SERVICE_ID UINT16_C(4)
#define PXA_CLOCK_SET_PERIOD UINT16_C(1)
#define PXA_CLOCK_NOW UINT16_C(2)
#define PXA_CLOCK_TICK UINT16_C(0x8001)
#define PXA_CLOCK_NOW_RESULT UINT16_C(0x8002)
#define PXA_CONFIG_SERVICE_VERSION UINT16_C(4)
#define PXA_SYSTEM_CONFIG_ENVIRONMENT UINT16_C(12)
#define PXA_SYSTEM_CONFIGURATION_LOCALE UINT16_C(1)
#define PXA_SYSTEM_CONFIGURATION_TEXT_DIRECTION UINT16_C(0x8002)
#define PXA_SYSTEM_LOCALE_MAX_BYTES 63u
#define PXA_ESP_HOST_CJK_FONT_PATH \
    CONFIG_PXA_MOUNT_POINT "/system/fonts/noto_sans_cjk_common.ttf"
#ifndef CONFIG_PXA_GUEST_STACK_SIZE
#define CONFIG_PXA_GUEST_STACK_SIZE (128 * 1024)
#endif
#ifndef CONFIG_PXA_HOST_MANAGED_HEAP_SIZE
#define CONFIG_PXA_HOST_MANAGED_HEAP_SIZE (16 * 1024)
#endif
#ifndef CONFIG_PXA_CALL_TIMEOUT_MS
#define CONFIG_PXA_CALL_TIMEOUT_MS 3000
#endif
#ifndef CONFIG_PXA_WATCHDOG_PERIOD_MS
#define CONFIG_PXA_WATCHDOG_PERIOD_MS 50
#endif
#define PXA_ESP_HOST_WATCHDOG_PERIOD_US \
    ((uint64_t)CONFIG_PXA_WATCHDOG_PERIOD_MS * UINT64_C(1000))
#ifndef PXA_ESP_ACCEPTANCE_PERF
#define PXA_ESP_ACCEPTANCE_PERF 0
#endif
#ifndef CONFIG_PXA_COMMAND_QUEUE_LENGTH
#define CONFIG_PXA_COMMAND_QUEUE_LENGTH 16
#endif
#ifndef CONFIG_PXA_RUNTIME_TASK_STACK_SIZE
#define CONFIG_PXA_RUNTIME_TASK_STACK_SIZE (12 * 1024)
#endif
#define PXA_ESP_HOST_MIN_AOT_TASK_STACK_SIZE (12 * 1024)
#ifndef CONFIG_PXA_RUNTIME_TASK_PRIORITY
#define CONFIG_PXA_RUNTIME_TASK_PRIORITY 4
#endif
#ifndef CONFIG_PXA_RUNTIME_TASK_AFFINITY
#define CONFIG_PXA_RUNTIME_TASK_AFFINITY tskNO_AFFINITY
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S31)
#define PXA_ESP_HOST_TARGET "esp32-s31"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define PXA_ESP_HOST_TARGET "esp32-s3"
#else
#error "PXA ESP host target is not defined for this IDF target"
#endif

/* The public permission projection owns one text buffer per field. */
typedef char pxa_esp_host_permission_text_must_fit[
    PXA_ESP_PACKAGE_PERMISSION_TEXT_BYTES <= PXA_HOST_PERMISSION_TEXT_MAX ? 1
                                                                          : -1];

static size_t runtime_task_stack_size(void) {
    size_t configured = CONFIG_PXA_RUNTIME_TASK_STACK_SIZE;

    /* Retain a small margin over the historical 10 KiB allocation while
     * keeping old sdkconfig files operational. */
    return configured < PXA_ESP_HOST_MIN_AOT_TASK_STACK_SIZE
               ? PXA_ESP_HOST_MIN_AOT_TASK_STACK_SIZE
               : configured;
}

typedef struct {
    uint8_t active;
    uint32_t prompt_id;
    pxa_component_t component;
    uint32_t request_id;
} pxa_esp_host_permission_prompt_t;

typedef struct {
    char identity[PXA_ESP_HOST_MAX_APP_ID];
    char name[PXA_HOST_PACKAGE_NAME_MAX];
    uint64_t main_instance_id;
    uint16_t permission_count;
    uint8_t package_active;
    uint8_t main_active;
    uint8_t volume_key_capture_active;
} pxa_esp_host_public_state_t;

typedef struct {
    pxa_runtime_t *runtime;
    pxa_component_t ui_component;

    pxa_activation_coordinator_t *coordinator;
    void *coordinator_workspace;
    void *plan_workspace;

    pxa_esp_services_t services;
    void *manifest_workspace_keep;
    uint8_t *encoded_keep;
    pxa_package_manifest_t *active_manifest;
    pxa_host_activation_arena_t activation_memory;
    char active_package_root[PXA_ESP_HOST_MAX_PATH];

    char active_identity[PXA_ESP_HOST_MAX_APP_ID];
    char active_name[PXA_HOST_PACKAGE_NAME_MAX];
    pxa_component_t active_component;
    uint64_t active_instance_id;
    pxa_component_t *active_components;
    size_t active_component_count;
    size_t active_component_capacity;
    uint32_t next_permission_prompt_id;
    pxa_esp_host_permission_prompt_t permission_prompt;

    pxa_component_t *active_jobs;
    uint64_t *job_stop_at_ms;
    uint64_t *job_force_stop_at_ms;
    pxa_scheduler_entry_t *active_work;
    pxa_work_result_t *work_result;
    int16_t activating_job_slot;
} pxa_esp_host_activation_t;

typedef struct {
    uint8_t initialized;
    uint8_t runtime_started;
    uint8_t runtime_starting;
    QueueHandle_t queue;
    TaskHandle_t runtime_task;
    esp_timer_handle_t clock_timer;
    esp_timer_handle_t watchdog_timer;
    pthread_t thread;

    void *runtime_workspace;
    pxa_wamr_engine_t *engine;
    void *engine_workspace;
    pxa_component_engine_t engine_ops;
    pxa_lvgl_ui_t *ui_adapter;
    void *ui_adapter_workspace;
    pxa_ui_backend_t ui_backend;
    lv_font_t *ui_body_font;
    lv_font_t *ui_title_font;
    lv_font_t *ui_caption_font;
    lv_font_t *ui_label_font;
    lv_font_t *ui_headline_font;
    lv_font_t *ui_display_font;
    pxa_lvgl_ui_theme_t ui_theme;
    uint32_t ui_theme_generation;
    uint32_t pending_ui_palette[PXA_UI_THEME_ROLE_COUNT];
    uint8_t ui_palette_pending;

    uint64_t last_clock_us;
    uint64_t next_maintenance_us;
    uint64_t next_lifecycle_us;
    uint8_t scheduler_maintenance_pending;
    pxa_ui_color_scheme_t color_scheme;
    pxa_ui_color_scheme_t pending_color_scheme;
    uint8_t color_scheme_pending;
    char locale[PXA_SYSTEM_LOCALE_MAX_BYTES + 1u];
    uint8_t locale_size;
    uint8_t text_direction;
    pxa_window_insets_t pending_safe_insets;
    pxa_window_insets_t pending_system_bar_insets;
    uint8_t window_insets_pending;
    pxa_window_insets_t safe_insets;
    pxa_window_insets_t system_bar_insets;
    uint8_t window_insets_valid;
    uint32_t display_shape;
    uint32_t corner_radii[4];
    uint64_t work_epoch;
    uint64_t last_instance_id;
    uint32_t next_unresponsive_prompt_id;
    uint32_t unresponsive_prompt_id;
    pxa_host_clock_slots_t clock_slots;
    pxa_host_pointer_mailbox_t pointer_mailbox;
    pxa_esp_host_public_state_t public_state;
    pxa_esp_host_activation_t activation;
} pxa_esp_host_t;

static pxa_esp_host_t g_host;
static pxa_esp_store_job_t *g_store_install;
static struct {
    uint32_t prompt_id;
    uint32_t component;
    uint32_t request_id;
    uint64_t instance_id;
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
    char name[PXA_ESP_PACKAGE_NAME_BYTES];
} g_store_uninstall;
static struct {
    uint32_t prompt_id;
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
} g_store_result;

static void copy_utf8_c_string(char *output, size_t capacity,
                               const char *value);

static void show_store_result(bool is_uninstall, const char *name,
                              const char *identity) {
    pxa_esp_ui_store_result_t result = {0};
    if (g_store_result.prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_store_result(g_store_result.prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        memset(&g_store_result, 0, sizeof(g_store_result));
    }
    result.prompt_id = ++g_host.activation.next_permission_prompt_id;
    if (result.prompt_id == 0)
        result.prompt_id = ++g_host.activation.next_permission_prompt_id;
    result.is_uninstall = is_uninstall ? 1u : 0u;
    copy_utf8_c_string(result.app_name, sizeof(result.app_name), name);
    g_store_result.prompt_id = result.prompt_id;
    if (!is_uninstall)
        snprintf(g_store_result.identity, sizeof(g_store_result.identity),
                 "%s", identity);
    pxa_esp_surface_runtime_modal_enter();
    if (!pxa_esp_ui_shell_post_store_result(&result)) {
        pxa_esp_surface_runtime_modal_leave();
        memset(&g_store_result, 0, sizeof(g_store_result));
    }
}
typedef struct {
    char filename[20];
    char app_id[65];
    char owner[PXA_ESP_PACKAGE_ID_BYTES];
} pxa_esp_store_download_entry_t;
#define PXA_ESP_STORE_DOWNLOAD_SLOTS 8u
static pxa_esp_store_download_entry_t g_store_downloads[PXA_ESP_STORE_DOWNLOAD_SLOTS];

static bool store_download_filename_valid(const char *name) {
    if (name == NULL || strlen(name) != 19u ||
        strncmp(name, ".store-", 7u) != 0 || strcmp(name + 15u, ".pxa") != 0)
        return false;
    for (size_t index = 7u; index < 15u; ++index)
        if (!((name[index] >= '0' && name[index] <= '9') ||
              (name[index] >= 'a' && name[index] <= 'f'))) return false;
    return true;
}

static bool store_registry_save(void) {
    const char *path = CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT "/.store-index";
    const char *temporary = CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT "/.store-index.tmp";
    FILE *file = fopen(temporary, "wb");
    bool saved;
    if (file == NULL) return false;
    saved = fwrite("PXADL1", 1u, 6u, file) == 6u &&
            fwrite(g_store_downloads, sizeof(g_store_downloads), 1u, file) == 1u &&
            fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) saved = false;
    if (saved) saved = rename(temporary, path) == 0;
    if (!saved) (void)unlink(temporary);
    return saved;
}

static void store_registry_load(void) {
    const char *path = CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT "/.store-index";
    FILE *file = fopen(path, "rb");
    char magic[6];
    memset(g_store_downloads, 0, sizeof(g_store_downloads));
    if (file == NULL) return;
    if (fread(magic, 1u, sizeof(magic), file) != sizeof(magic) ||
        memcmp(magic, "PXADL1", sizeof(magic)) != 0 ||
        fread(g_store_downloads, sizeof(g_store_downloads), 1u, file) != 1u ||
        fgetc(file) != EOF) {
        memset(g_store_downloads, 0, sizeof(g_store_downloads));
    }
    (void)fclose(file);
    for (size_t index = 0; index < PXA_ESP_STORE_DOWNLOAD_SLOTS; ++index) {
        pxa_esp_store_download_entry_t *entry = &g_store_downloads[index];
        char filename[20];
        char full_path[160];
        struct stat metadata;
        if (entry->filename[0] == '\0') continue;
        if (memchr(entry->filename, '\0', sizeof(entry->filename)) == NULL ||
            memchr(entry->app_id, '\0', sizeof(entry->app_id)) == NULL ||
            memchr(entry->owner, '\0', sizeof(entry->owner)) == NULL ||
            !store_download_filename_valid(entry->filename) ||
            entry->app_id[0] == '\0' || entry->owner[0] == '\0') {
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        snprintf(filename, sizeof(filename), "%s", entry->filename);
        if (snprintf(full_path, sizeof(full_path), "%s/%s/%s",
                     CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
                     filename) >= (int)sizeof(full_path) ||
            stat(full_path, &metadata) != 0 || !S_ISREG(metadata.st_mode))
            memset(entry, 0, sizeof(*entry));
    }
}

static int store_registry_keep(const char *filename, void *context) {
    (void)context;
    for (size_t index = 0; index < PXA_ESP_STORE_DOWNLOAD_SLOTS; ++index)
        if (strcmp(g_store_downloads[index].filename, filename) == 0) return 1;
    return 0;
}
static pxa_host_runtime_event_fn g_runtime_event_callback;
static void *g_runtime_event_context;
static portMUX_TYPE g_runtime_event_lock = portMUX_INITIALIZER_UNLOCKED;
static pxa_host_system_request_fn g_system_request_callback;
static void *g_system_request_context;
static portMUX_TYPE g_system_request_lock = portMUX_INITIALIZER_UNLOCKED;
static pxa_host_window_changed_fn g_window_changed_callback;
static void *g_window_changed_context;
static portMUX_TYPE g_window_changed_lock = portMUX_INITIALIZER_UNLOCKED;

const lv_font_t *pxa_esp_host_ui_body_font(void) {
    return g_host.ui_body_font;
}

const lv_font_t *pxa_esp_host_ui_title_font(void) {
    return g_host.ui_title_font;
}

uint32_t pxa_esp_host_ui_color(uint8_t index) {
    return index < PXA_UI_THEME_ROLE_COUNT ?
           g_host.ui_theme.rgba[index] : 0u;
}

static lv_font_t *load_ui_font(uint16_t size,
                                const lv_font_t *symbol_fallback) {
#if defined(CONFIG_LV_USE_FREETYPE) && CONFIG_LV_USE_FREETYPE
    lv_font_t *font = lv_freetype_font_create(
        PXA_ESP_HOST_CJK_FONT_PATH, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if (font != NULL) font->fallback = symbol_fallback;
    return font;
#else
    (void)size;
    (void)symbol_fallback;
    return NULL;
#endif
}

static void release_ui_fonts(void) {
#if defined(CONFIG_LV_USE_FREETYPE) && CONFIG_LV_USE_FREETYPE
    if (g_host.ui_display_font != NULL)
        lv_freetype_font_delete(g_host.ui_display_font);
    if (g_host.ui_headline_font != NULL)
        lv_freetype_font_delete(g_host.ui_headline_font);
    if (g_host.ui_title_font != NULL)
        lv_freetype_font_delete(g_host.ui_title_font);
    if (g_host.ui_body_font != NULL)
        lv_freetype_font_delete(g_host.ui_body_font);
    if (g_host.ui_label_font != NULL)
        lv_freetype_font_delete(g_host.ui_label_font);
    if (g_host.ui_caption_font != NULL)
        lv_freetype_font_delete(g_host.ui_caption_font);
#endif
    g_host.ui_display_font = NULL;
    g_host.ui_headline_font = NULL;
    g_host.ui_title_font = NULL;
    g_host.ui_body_font = NULL;
    g_host.ui_label_font = NULL;
    g_host.ui_caption_font = NULL;
}

typedef struct {
    char app_id[PXA_HOST_PACKAGE_ID_MAX];
    pxa_component_t component;
    uint32_t request_id;
    pxa_status_t status;
    size_t payload_size;
    uint8_t payload[];
} pxa_host_system_response_t;

typedef struct {
    char app_id[PXA_HOST_PACKAGE_ID_MAX];
    pxa_component_t component;
    uint32_t request_id;
    uint16_t opcode;
    size_t payload_size;
    uint8_t payload[];
} pxa_host_system_event_t;

static int drain_active_events(void);

static void notify_runtime_event(pxa_host_runtime_event_t event,
                                 const char *identity) {
    pxa_host_runtime_event_fn callback;
    void *context;
    if (identity == NULL || identity[0] == '\0') return;
    portENTER_CRITICAL(&g_runtime_event_lock);
    callback = g_runtime_event_callback;
    context = g_runtime_event_context;
    portEXIT_CRITICAL(&g_runtime_event_lock);
    if (callback != NULL) callback(context, event, identity);
}

static int system_caller_for_component(pxa_component_t component,
                                       pxa_host_system_caller_t *output) {
    const pxa_package_manifest_t *manifest =
        g_host.activation.active_manifest;
    uint16_t index;
    if (output == NULL || manifest == NULL ||
        manifest->management_key_id == NULL || manifest->app_id.data == NULL ||
        manifest->app_id.size == 0 ||
        manifest->app_id.size >= sizeof(output->app_id) ||
        g_host.activation.coordinator == NULL) {
        return 0;
    }
    memset(output, 0, sizeof(*output));
    output->runtime_component = component;
    memcpy(output->publisher_root, manifest->management_key_id,
           sizeof(output->publisher_root));
    memcpy(output->app_id, manifest->app_id.data, manifest->app_id.size);
    for (index = 0; index < manifest->component_count; ++index) {
        const pxa_bytes_t id = manifest->components[index].id;
        pxa_component_t found = PXA_COMPONENT_INVALID;
        uint64_t instance_id = 0;
        if (id.data == NULL || id.size == 0 ||
            id.size >= sizeof(output->component_id)) {
            continue;
        }
        if (pxa_activation_find(g_host.activation.coordinator, id,
                                &instance_id, &found) == PXA_STATUS_OK &&
            found == component) {
            memcpy(output->component_id, id.data, id.size);
            return 1;
        }
    }
    return 0;
}

static pxa_status_t host_system_control(
    void *context, pxa_runtime_t *runtime, pxa_component_t component,
    const pxa_message_view_t *message) {
    pxa_host_system_request_fn callback;
    void *callback_context;
    pxa_host_system_caller_t caller;
    pxa_status_t status;
    (void)context;
    if (runtime == NULL || message == NULL || message->request_id == 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (message->opcode != PXA_HOST_SYSTEM_INTENT_START &&
        message->opcode != PXA_HOST_SYSTEM_SERVICE_INVOKE &&
        message->opcode != PXA_HOST_SYSTEM_TOPIC_PUBLISH &&
        message->opcode != PXA_HOST_SYSTEM_TOPIC_SUBSCRIBE &&
        message->opcode != PXA_HOST_SYSTEM_TOPIC_UNSUBSCRIBE &&
        message->opcode != PXA_HOST_SYSTEM_SERVICE_REGISTER &&
        message->opcode != PXA_HOST_SYSTEM_SERVICE_UNREGISTER &&
        message->opcode != PXA_HOST_SYSTEM_SERVICE_COMPLETE)
        return PXA_STATUS_UNSUPPORTED;
    if (!system_caller_for_component(component, &caller))
        return PXA_STATUS_NOT_FOUND;
    status = pxa_request_begin(runtime, component, message->request_id,
                               PXA_HOST_SYSTEM_SERVICE_ID, message->opcode, 0);
    if (status != PXA_STATUS_OK) return status;
    portENTER_CRITICAL(&g_system_request_lock);
    callback = g_system_request_callback;
    callback_context = g_system_request_context;
    portEXIT_CRITICAL(&g_system_request_lock);
    if (callback != NULL &&
        callback(callback_context, &caller, message->opcode,
                 message->request_id, message->payload.data,
                 message->payload.size)) {
        return PXA_STATUS_OK;
    }
    status = pxa_request_complete(runtime, component, message->request_id,
                                  PXA_STATUS_UNAVAILABLE, NULL, 0);
    if (status != PXA_STATUS_OK)
        (void)pxa_request_cancel(runtime, component, message->request_id);
    return PXA_STATUS_OK;
}

static void complete_system_request(pxa_host_system_response_t *response) {
    pxa_status_t status;
    if (response == NULL) return;
    if (g_host.activation.runtime != NULL &&
        strcmp(g_host.activation.active_identity, response->app_id) == 0 &&
        pxa_request_is_active(g_host.activation.runtime, response->component,
                              response->request_id)) {
        status = pxa_request_complete(
            g_host.activation.runtime, response->component,
            response->request_id, response->status,
            response->status == PXA_STATUS_OK ? response->payload : NULL,
            response->status == PXA_STATUS_OK ? response->payload_size : 0);
        if (status != PXA_STATUS_OK) {
            (void)pxa_request_cancel(g_host.activation.runtime,
                                     response->component,
                                     response->request_id);
        }
    }
    free(response);
}

static void post_system_event(pxa_host_system_event_t *event) {
    pxa_component_t component;
    if (event == NULL) return;
    if (g_host.activation.runtime != NULL &&
        strcmp(g_host.activation.active_identity, event->app_id) == 0) {
        component = event->component == PXA_COMPONENT_INVALID
                        ? g_host.activation.active_component
                        : event->component;
        if (component == PXA_COMPONENT_INVALID) {
            free(event);
            return;
        }
        (void)pxa_event_post_message(
            g_host.activation.runtime, component,
            PXA_HOST_SYSTEM_SERVICE_ID, event->opcode, event->request_id,
            (pxa_bytes_t){event->payload, event->payload_size}, 1, 0);
        drain_active_events();
    }
    free(event);
}
static portMUX_TYPE g_pointer_mailbox_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_clock_slots_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_public_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_process_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_net_completion_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_wamr_watchdog_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t g_net_completion_ready;

static void enter_wamr_watchdog_critical(void *context) {
    portENTER_CRITICAL((portMUX_TYPE *)context);
}

static void leave_wamr_watchdog_critical(void *context) {
    portEXIT_CRITICAL((portMUX_TYPE *)context);
}

static void *esp_alloc(size_t size) {
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return memory;
}

static void *ipc_alloc(void *context, size_t size) {
    (void)context;
    return esp_alloc(size);
}

static void ipc_release(void *context, void *memory) {
    (void)context;
    free(memory);
}

/* Keep the raw 4-byte-aligned ESP allocation so realloc can grow it in place,
 * while exposing the 8-byte-aligned address required by WAMR. */
typedef struct {
    void *raw_memory;
    size_t size;
} pxa_esp_wamr_allocation_header_t;

_Static_assert((PXA_ESP_WAMR_ALLOC_ALIGNMENT &
                (PXA_ESP_WAMR_ALLOC_ALIGNMENT - 1u)) == 0,
               "WAMR allocation alignment must be a power of two");
_Static_assert(PXA_ESP_WAMR_ALLOC_ALIGNMENT >=
                   _Alignof(pxa_esp_wamr_allocation_header_t),
               "WAMR allocation alignment must cover its private header");

static size_t esp_wamr_allocation_size(size_t size) {
    const size_t overhead = sizeof(pxa_esp_wamr_allocation_header_t) +
                            PXA_ESP_WAMR_ALLOC_ALIGNMENT - 1u;
    return size > SIZE_MAX - overhead ? 0 : size + overhead;
}

static void *esp_wamr_aligned_memory(void *raw_memory) {
    const uintptr_t first =
        (uintptr_t)raw_memory + sizeof(pxa_esp_wamr_allocation_header_t);
    const uintptr_t aligned =
        (first + PXA_ESP_WAMR_ALLOC_ALIGNMENT - 1u) &
        ~(uintptr_t)(PXA_ESP_WAMR_ALLOC_ALIGNMENT - 1u);
    return (void *)aligned;
}

static void *esp_wamr_alloc(void *context, size_t size) {
    const size_t allocation_size = esp_wamr_allocation_size(size);
    void *raw_memory;
    (void)context;
    if (allocation_size == 0) return NULL;
    raw_memory = heap_caps_malloc(allocation_size,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw_memory == NULL) return NULL;
    {
        void *memory = esp_wamr_aligned_memory(raw_memory);
        pxa_esp_wamr_allocation_header_t *header =
            (pxa_esp_wamr_allocation_header_t *)memory - 1;
        header->raw_memory = raw_memory;
        header->size = size;
        return memory;
    }
}

static void *esp_wamr_realloc(void *context, void *memory, size_t size) {
    pxa_esp_wamr_allocation_header_t *previous_header;
    pxa_esp_wamr_allocation_header_t *resized_header;
    size_t allocation_size;
    size_t previous_offset;
    size_t copy_size;
    void *resized_raw;
    void *resized_memory;
    (void)context;
    if (memory == NULL) return esp_wamr_alloc(context, size);
    if (size == 0) {
        previous_header =
            (pxa_esp_wamr_allocation_header_t *)memory - 1;
        heap_caps_free(previous_header->raw_memory);
        return NULL;
    }
    allocation_size = esp_wamr_allocation_size(size);
    if (allocation_size == 0) return NULL;
    previous_header = (pxa_esp_wamr_allocation_header_t *)memory - 1;
    previous_offset =
        (size_t)((uint8_t *)memory -
                 (uint8_t *)previous_header->raw_memory);
    copy_size = previous_header->size < size ? previous_header->size : size;
    resized_raw = heap_caps_realloc(previous_header->raw_memory,
                                    allocation_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resized_raw == NULL) return NULL;
    resized_memory = esp_wamr_aligned_memory(resized_raw);
    resized_header =
        (pxa_esp_wamr_allocation_header_t *)resized_memory - 1;
    memmove(resized_memory, (uint8_t *)resized_raw + previous_offset,
            copy_size);
    resized_header->raw_memory = resized_raw;
    resized_header->size = size;
    return resized_memory;
}

static void esp_wamr_free(void *context, void *memory) {
    pxa_esp_wamr_allocation_header_t *header;
    (void)context;
    if (memory == NULL) return;
    header = (pxa_esp_wamr_allocation_header_t *)memory - 1;
    heap_caps_free(header->raw_memory);
}

static void *esp_activation_alloc(void *context, size_t size) {
    (void)context;
    return esp_alloc(size);
}

static void esp_activation_free(void *context, void *memory) {
    (void)context;
    free(memory);
}

static void *activation_alloc(size_t size) {
    return pxa_host_activation_arena_allocate(
        &g_host.activation.activation_memory, size);
}

static void *activation_workspace_alloc(void *context, size_t size) {
    return pxa_host_activation_arena_allocate(
        (pxa_host_activation_arena_t *)context, size);
}

static int activation_memory_begin(void *manifest_workspace,
                                   size_t manifest_workspace_size,
                                   void *encoded, size_t encoded_size) {
    return pxa_host_activation_arena_begin(
        &g_host.activation.activation_memory, manifest_workspace,
        manifest_workspace_size, encoded, encoded_size);
}

static void activation_memory_release_all(void) {
    pxa_host_activation_arena_release_all(&g_host.activation.activation_memory);
}

static void activation_state_reset(void) {
    pxa_host_activation_allocate_fn allocate =
        g_host.activation.activation_memory.allocate;
    pxa_host_activation_release_fn release =
        g_host.activation.activation_memory.release;
    void *allocator_context =
        g_host.activation.activation_memory.allocator_context;
    memset(&g_host.activation, 0, sizeof(g_host.activation));
    if (allocate != NULL && release != NULL) {
        (void)pxa_host_activation_arena_init(
            &g_host.activation.activation_memory, allocator_context,
            allocate, release);
    }
}

static void *esp_ui_alloc(void *context, size_t size) {
    (void)context;
    return esp_alloc(size);
}

static void esp_ui_free(void *context, void *memory) {
    (void)context;
    free(memory);
}

static void *esp_artifact_alloc(void *context, size_t size) {
    (void)context;
    return esp_alloc(size);
}

static void esp_artifact_free(void *context, void *memory) {
    (void)context;
    free(memory);
}

static uint64_t host_now_us(void *context) {
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

static uint64_t host_clock_ms(void *context) {
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* Instance IDs are process-monotonic so queued events from a stopped
 * activation cannot become valid again after a Package switch. */
static uint64_t allocate_instance_id(void) {
    uint64_t instance_id = ++g_host.last_instance_id;
    if (instance_id == 0) instance_id = ++g_host.last_instance_id;
    return instance_id;
}

static void public_state_snapshot(pxa_esp_host_public_state_t *output) {
    if (output == NULL) return;
    portENTER_CRITICAL(&g_public_state_lock);
    *output = g_host.public_state;
    portEXIT_CRITICAL(&g_public_state_lock);
}

static void publish_active_state(void) {
    pxa_esp_host_public_state_t state;
    memset(&state, 0, sizeof(state));
    snprintf(state.identity, sizeof(state.identity), "%s",
             g_host.activation.active_identity);
    snprintf(state.name, sizeof(state.name), "%s",
             g_host.activation.active_name);
    state.main_instance_id = g_host.activation.active_instance_id;
    state.permission_count =
        g_host.activation.services.declaration_count;
    state.package_active = 1;
    state.main_active = 1;
    portENTER_CRITICAL(&g_public_state_lock);
    g_host.public_state = state;
    portEXIT_CRITICAL(&g_public_state_lock);
    notify_runtime_event(PXA_HOST_RUNTIME_STARTED, state.identity);
}

static void publish_main_stopped(void) {
    char identity[PXA_ESP_HOST_MAX_APP_ID];
    snprintf(identity, sizeof(identity), "%s",
             g_host.activation.active_identity);
    portENTER_CRITICAL(&g_public_state_lock);
    g_host.public_state.main_active = 0;
    g_host.public_state.main_instance_id = 0;
    g_host.public_state.volume_key_capture_active = 0;
    portEXIT_CRITICAL(&g_public_state_lock);
    notify_runtime_event(PXA_HOST_RUNTIME_STOPPED, identity);
}

static void unpublish_active_state(void) {
    portENTER_CRITICAL(&g_public_state_lock);
    memset(&g_host.public_state, 0, sizeof(g_host.public_state));
    portEXIT_CRITICAL(&g_public_state_lock);
}

static void wake_runtime_thread(void) {
    TaskHandle_t task;
    portENTER_CRITICAL(&g_process_state_lock);
    task = g_host.runtime_task;
    portEXIT_CRITICAL(&g_process_state_lock);
    if (task != NULL) xTaskNotifyGive(task);
}

static void on_surface_release_ready(void *context) {
    (void)context;
    wake_runtime_thread();
}

static int post_command(const pxa_esp_host_command_t *command) {
    int posted = g_host.queue != NULL &&
                 xQueueSendToBack(g_host.queue, command, 0) == pdTRUE;
    if (posted) wake_runtime_thread();
    return posted;
}

static void store_install_notify(pxa_esp_store_job_t *job, uint8_t phase) {
    pxa_esp_host_command_t command = {0};
    command.type = (uint8_t)PXA_ESP_HOST_CMD_STORE_INSTALL;
    command.payload.store_install.job = job;
    command.payload.store_install.phase = phase;
    if (phase == 4u)
        command.payload.store_install.downloaded_bytes = job->downloaded_bytes;
    if (g_host.queue != NULL &&
        xQueueSendToBack(g_host.queue, &command,
                         phase == 4u ? 0 : portMAX_DELAY) == pdTRUE)
        wake_runtime_thread();
}

static pxa_esp_store_download_entry_t *store_download_find(
    const uint8_t *filename, size_t length) {
    if (filename == NULL || length != 19u) return NULL;
    for (size_t index = 0; index < PXA_ESP_STORE_DOWNLOAD_SLOTS; ++index) {
        pxa_esp_store_download_entry_t *entry = &g_store_downloads[index];
        if (strlen(entry->filename) == length &&
            memcmp(entry->filename, filename, length) == 0 &&
            strcmp(entry->owner, g_host.activation.active_identity) == 0)
            return entry;
    }
    return NULL;
}

static pxa_esp_store_download_entry_t *store_download_free(void) {
    for (size_t index = 0; index < PXA_ESP_STORE_DOWNLOAD_SLOTS; ++index)
        if (g_store_downloads[index].filename[0] == '\0')
            return &g_store_downloads[index];
    return NULL;
}

typedef struct {
    uint8_t payload[3072];
    size_t length;
    size_t count;
    const char *app_id;
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
} store_inventory_t;

typedef struct {
    const char *app_id;
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
    char name[PXA_ESP_PACKAGE_NAME_BYTES];
    char version[PXA_ESP_PACKAGE_VERSION_BYTES];
    uint8_t matches;
    bool built_in;
} store_uninstall_target_t;

static bool store_uninstall_target_visit(const pxa_esp_package_record_t *record,
                                         void *context) {
    store_uninstall_target_t *target = context;
    if (!record->installed || strcmp(record->app_id, target->app_id) != 0)
        return true;
    if (++target->matches != 1u) return false;
    snprintf(target->identity, sizeof(target->identity), "%s", record->id);
    snprintf(target->name, sizeof(target->name), "%s", record->name);
    snprintf(target->version, sizeof(target->version), "%s", record->version);
    target->built_in = record->built_in;
    return true;
}

static bool store_inventory_visit(const pxa_esp_package_record_t *record,
                                  void *context) {
    store_inventory_t *inventory = context;
    size_t app_length, version_length, identity_length;
    if (!record->installed) return true;
    if (inventory->app_id != NULL) {
        if (strcmp(record->app_id, inventory->app_id) == 0)
            snprintf(inventory->identity, sizeof(inventory->identity), "%s", record->id);
        return inventory->identity[0] == '\0';
    }
    app_length = strlen(record->app_id);
    version_length = strlen(record->version);
    identity_length = strlen(record->id);
    if (app_length > 64u || version_length > 31u || identity_length > 129u ||
        inventory->length + 12u + app_length + version_length + identity_length >
            sizeof(inventory->payload) || inventory->count == 12u) return false;
    uint8_t *out = inventory->payload + inventory->length;
    out[0] = (uint8_t)app_length;
    out[1] = (uint8_t)version_length;
    out[2] = (uint8_t)identity_length;
    for (size_t byte = 0; byte < 8u; ++byte)
        out[3u + byte] = (uint8_t)(record->release_sequence >> (byte * 8u));
    out[11] = record->built_in ? 0u : 1u;
    memcpy(out + 12u, record->app_id, app_length);
    memcpy(out + 12u + app_length, record->version, version_length);
    memcpy(out + 12u + app_length + version_length, record->id, identity_length);
    inventory->length += 12u + app_length + version_length + identity_length;
    inventory->count++;
    return true;
}

static pxa_status_t store_complete_now(pxa_runtime_t *runtime,
                                       pxa_component_t component,
                                       const pxa_message_view_t *message,
                                       const void *payload, size_t length) {
    pxa_status_t result = pxa_request_begin(runtime, component, message->request_id,
                            PXA_STORE_INSTALL_SERVICE_ID, message->opcode, 0);
    if (result != PXA_STATUS_OK) return result;
    return pxa_request_complete(runtime, component, message->request_id,
                                PXA_STATUS_OK, payload, length);
}

static void copy_utf8_c_string(char *output, size_t capacity,
                               const char *value);

static pxa_status_t host_store_install_control(
    void *context, pxa_runtime_t *runtime, pxa_component_t component,
    const pxa_message_view_t *message) {
    pxa_esp_store_job_t *job;
    pxa_status_t status;
    (void)context;
    if (message == NULL || message->request_id == 0)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (message->opcode == PXA_STORE_INSTALLED_LIST_REQUEST) {
        store_inventory_t *inventory;
        if (message->payload.size != 0) return PXA_STATUS_INVALID_ARGUMENT;
        inventory = calloc(1, sizeof(*inventory));
        if (inventory == NULL) return PXA_STATUS_RESOURCE_LIMIT;
        inventory->length = 1u;
        (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_MANAGED,
                                          store_inventory_visit, inventory);
        inventory->payload[0] = (uint8_t)inventory->count;
        status = store_complete_now(runtime, component, message,
                                    inventory->payload, inventory->length);
        free(inventory);
        return status;
    }
    if (message->opcode == PXA_STORE_DOWNLOAD_LIST_REQUEST) {
        uint8_t payload[1u + PXA_ESP_STORE_DOWNLOAD_SLOTS * 85u] = {0};
        size_t length = 1u;
        if (message->payload.size != 0) return PXA_STATUS_INVALID_ARGUMENT;
        for (size_t index = 0; index < PXA_ESP_STORE_DOWNLOAD_SLOTS; ++index) {
            pxa_esp_store_download_entry_t *entry = &g_store_downloads[index];
            if (entry->filename[0] == '\0' ||
                strcmp(entry->owner, g_host.activation.active_identity) != 0) continue;
            const size_t app_length = strlen(entry->app_id);
            payload[length++] = (uint8_t)app_length;
            memcpy(payload + length, entry->filename, 19u);
            length += 19u;
            memcpy(payload + length, entry->app_id, app_length);
            length += app_length;
            ++payload[0];
        }
        return store_complete_now(runtime, component, message, payload, length);
    }
    if (message->opcode == PXA_STORE_DELETE_REQUEST) {
        pxa_esp_store_download_entry_t *entry;
        char path[160];
        if (g_store_install != NULL) return PXA_STATUS_BUSY;
        entry = store_download_find(message->payload.data, message->payload.size);
        if (entry == NULL) return PXA_STATUS_NOT_FOUND;
        if (snprintf(path, sizeof(path), "%s/%s/%s", CONFIG_PXA_MOUNT_POINT,
                     CONFIG_PXA_STATE_ROOT, entry->filename) >= (int)sizeof(path))
            return PXA_STATUS_INVALID_ARGUMENT;
        if (unlink(path) != 0) return PXA_STATUS_IO_ERROR;
        memset(entry, 0, sizeof(*entry));
        (void)store_registry_save();
        return store_complete_now(runtime, component, message, NULL, 0);
    }
    if (message->opcode == PXA_STORE_LAUNCH_REQUEST) {
        store_inventory_t inventory = {0};
        char app_id[65];
        if (message->payload.size == 0 || message->payload.size > 64u)
            return PXA_STATUS_INVALID_ARGUMENT;
        memcpy(app_id, message->payload.data, message->payload.size);
        app_id[message->payload.size] = '\0';
        inventory.app_id = app_id;
        (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_RUNNABLE,
                                          store_inventory_visit, &inventory);
        if (inventory.identity[0] == '\0') return PXA_STATUS_NOT_FOUND;
        status = store_complete_now(runtime, component, message, NULL, 0);
        if (status == PXA_STATUS_OK)
            (void)pxa_host_request_launch(inventory.identity);
        return status;
    }
    if (message->opcode == PXA_STORE_UNINSTALL_REQUEST) {
        store_uninstall_target_t target = {0};
        pxa_esp_ui_permission_prompt_t prompt = {0};
        char app_id[65];
        if (message->payload.size == 0 || message->payload.size > 64u ||
            memchr(message->payload.data, '\0', message->payload.size) != NULL)
            return PXA_STATUS_INVALID_ARGUMENT;
        if (g_store_install != NULL || g_store_uninstall.prompt_id != 0 ||
            g_host.activation.permission_prompt.active)
            return PXA_STATUS_BUSY;
        memcpy(app_id, message->payload.data, message->payload.size);
        app_id[message->payload.size] = '\0';
        target.app_id = app_id;
        (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_MANAGED,
                                          store_uninstall_target_visit, &target);
        if (target.matches == 0u) return PXA_STATUS_NOT_FOUND;
        if (target.matches != 1u || target.built_in ||
            strcmp(target.identity, g_host.activation.active_identity) == 0)
            return PXA_STATUS_DENIED;
        status = pxa_request_begin(runtime, component, message->request_id,
                                   PXA_STORE_INSTALL_SERVICE_ID,
                                   PXA_STORE_UNINSTALL_REQUEST, 0);
        if (status != PXA_STATUS_OK) return status;
        prompt.prompt_id = ++g_host.activation.next_permission_prompt_id;
        if (prompt.prompt_id == 0)
            prompt.prompt_id = ++g_host.activation.next_permission_prompt_id;
        prompt.is_uninstall = 1u;
        copy_utf8_c_string(prompt.app_name, sizeof(prompt.app_name), target.name);
        const bool chinese = g_host.locale[0] == 'z' && g_host.locale[1] == 'h';
        snprintf(prompt.permission_name, sizeof(prompt.permission_name),
                 chinese ? "卸载 %.36s (%.20s)？" : "Uninstall %.36s (%.20s)?",
                 app_id, target.version);
        snprintf(prompt.scope, sizeof(prompt.scope),
                 chinese ? "确认后将从设备中删除此应用" :
                           "This app will be removed from this device");
        g_store_uninstall.prompt_id = prompt.prompt_id;
        g_store_uninstall.component = component;
        g_store_uninstall.request_id = message->request_id;
        g_store_uninstall.instance_id = g_host.activation.active_instance_id;
        snprintf(g_store_uninstall.identity, sizeof(g_store_uninstall.identity),
                 "%s", target.identity);
        copy_utf8_c_string(g_store_uninstall.name,
                           sizeof(g_store_uninstall.name), target.name);
        pxa_esp_surface_runtime_modal_enter();
        if (!pxa_esp_ui_shell_post_permission_prompt(&prompt)) {
            pxa_esp_surface_runtime_modal_leave();
            memset(&g_store_uninstall, 0, sizeof(g_store_uninstall));
            (void)pxa_request_cancel(runtime, component, message->request_id);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        return PXA_STATUS_OK;
    }
    if (message->opcode == PXA_STORE_INSTALL_FILE_REQUEST) {
        pxa_esp_store_download_entry_t *entry;
        if (g_store_install != NULL || g_store_uninstall.prompt_id != 0)
            return PXA_STATUS_BUSY;
        entry = store_download_find(message->payload.data, message->payload.size);
        if (entry == NULL) return PXA_STATUS_DENIED;
        job = calloc(1, sizeof(*job));
        if (job == NULL) return PXA_STATUS_RESOURCE_LIMIT;
        snprintf(job->filename, sizeof(job->filename), "%s", entry->filename);
        snprintf(job->app_id, sizeof(job->app_id), "%s", entry->app_id);
        status = pxa_request_begin(runtime, component, message->request_id,
                                   PXA_STORE_INSTALL_SERVICE_ID,
                                   PXA_STORE_INSTALL_FILE_REQUEST, 0);
        if (status != PXA_STATUS_OK) { free(job); return status; }
        job->component = component;
        job->request_id = message->request_id;
        job->instance_id = g_host.activation.active_instance_id;
        job->separate = 2u;
        job->notify = store_install_notify;
        g_store_install = job;
        if (xTaskCreate(pxa_esp_store_worker, "pxa-store-install",
                        PXA_STORE_WORKER_STACK_BYTES,
                        job, 3, &job->worker) != pdPASS) {
            g_store_install = NULL;
            (void)pxa_request_cancel(runtime, component, message->request_id);
            free(job);
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        return PXA_STATUS_OK;
    }
    if (message->opcode != PXA_STORE_INSTALL_REQUEST &&
        message->opcode != PXA_STORE_DOWNLOAD_REQUEST)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (g_store_install != NULL || g_store_uninstall.prompt_id != 0)
        return PXA_STATUS_BUSY;
    if (message->opcode == PXA_STORE_DOWNLOAD_REQUEST && store_download_free() == NULL)
        return PXA_STATUS_RESOURCE_LIMIT;
    job = calloc(1, sizeof(*job));
    if (job == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    if (!pxa_esp_store_job_decode(job, message->payload.data,
                                  message->payload.size)) {
        ESP_LOGW(PXA_ESP_HOST_TAG, "Store install request rejected: invalid payload");
        free(job);
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    status = pxa_request_begin(runtime, component, message->request_id,
                               PXA_STORE_INSTALL_SERVICE_ID,
                               message->opcode, 0);
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(PXA_ESP_HOST_TAG, "Store request allocation failed: status=%ld",
                 (long)status);
        free(job);
        return status;
    }
    job->component = component;
    job->request_id = message->request_id;
    job->separate = message->opcode == PXA_STORE_DOWNLOAD_REQUEST;
    job->instance_id = g_host.activation.active_instance_id;
    job->notify = store_install_notify;
    g_store_install = job;
    ESP_LOGI(PXA_ESP_HOST_TAG, "Store worker start: internal_free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (xTaskCreate(pxa_esp_store_worker, "pxa-store-install",
                    PXA_STORE_WORKER_STACK_BYTES,
                    job, 3, &job->worker) != pdPASS) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Store worker allocation failed: internal_free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        g_store_install = NULL;
        (void)pxa_request_cancel(runtime, component, message->request_id);
        free(job);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    return PXA_STATUS_OK;
}

static int post_maintenance(pxa_esp_host_command_type_t type) {
    pxa_esp_host_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)type;
    return post_command(&command);
}

/* Lifecycle work must not be lost merely because the regular UI command queue
 * is full. The timer only marks it due; the Core owner thread consumes it. */
static void mark_scheduler_maintenance_due(void) {
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.scheduler_maintenance_pending = 1;
    portEXIT_CRITICAL(&g_process_state_lock);
    wake_runtime_thread();
}

static int take_scheduler_maintenance_due(void) {
    int pending;
    portENTER_CRITICAL(&g_process_state_lock);
    pending = g_host.scheduler_maintenance_pending != 0;
    g_host.scheduler_maintenance_pending = 0;
    portEXIT_CRITICAL(&g_process_state_lock);
    return pending;
}

static int take_pending_color_scheme(pxa_ui_color_scheme_t *color_scheme) {
    int pending;
    portENTER_CRITICAL(&g_process_state_lock);
    pending = g_host.color_scheme_pending != 0;
    if (pending && color_scheme != NULL)
        *color_scheme = g_host.pending_color_scheme;
    g_host.color_scheme_pending = 0;
    portEXIT_CRITICAL(&g_process_state_lock);
    return pending;
}

static int take_pending_ui_palette(uint32_t rgba[PXA_UI_THEME_ROLE_COUNT]) {
    int pending;
    portENTER_CRITICAL(&g_process_state_lock);
    pending = g_host.ui_palette_pending != 0;
    if (pending) memcpy(rgba, g_host.pending_ui_palette,
                        sizeof(g_host.pending_ui_palette));
    g_host.ui_palette_pending = 0;
    portEXIT_CRITICAL(&g_process_state_lock);
    return pending;
}

static int take_pending_window_insets(pxa_window_insets_t *safe_insets,
                                      pxa_window_insets_t *system_bar_insets) {
    int pending;
    portENTER_CRITICAL(&g_process_state_lock);
    pending = g_host.window_insets_pending != 0;
    if (pending && safe_insets != NULL)
        *safe_insets = g_host.pending_safe_insets;
    if (pending && system_bar_insets != NULL)
        *system_bar_insets = g_host.pending_system_bar_insets;
    g_host.window_insets_pending = 0;
    portEXIT_CRITICAL(&g_process_state_lock);
    return pending;
}

static void copy_utf8_c_string(char *output, size_t capacity,
                               const char *value) {
    size_t size;
    if (output == NULL || capacity == 0) return;
    if (value == NULL) {
        output[0] = '\0';
        return;
    }
    size = strnlen(value, capacity - 1);
    if (value[size] != '\0') {
        while (size != 0 && ((uint8_t)value[size] & 0xc0u) == 0x80u) --size;
    }
    memcpy(output, value, size);
    output[size] = '\0';
}

static void copy_permission_prompt_text(char *output, size_t capacity,
                                        pxa_bytes_t value,
                                        const char *empty_value) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t offset = 0;
    int printable = 1;
    if (output == NULL || capacity == 0) return;
    if (value.size == 0) {
        copy_utf8_c_string(output, capacity, empty_value);
        return;
    }
    for (index = 0; index < value.size; ++index) {
        if (value.data[index] < 0x20 || value.data[index] > 0x7e) {
            printable = 0;
            break;
        }
    }
    if (printable) {
        size_t copy_size = value.size < capacity - 1 ? value.size : capacity - 1;
        memcpy(output, value.data, copy_size);
        if (copy_size + 3 < capacity && copy_size < value.size) {
            output[copy_size++] = '.';
            output[copy_size++] = '.';
            output[copy_size++] = '.';
        }
        output[copy_size] = '\0';
        return;
    }
    if (capacity > 1) output[offset++] = '0';
    if (capacity > 2) output[offset++] = 'x';
    for (index = 0; index < value.size && offset + 2 < capacity; ++index) {
        output[offset++] = digits[value.data[index] >> 4];
        output[offset++] = digits[value.data[index] & 0x0f];
    }
    output[offset] = '\0';
}

static void copy_permission_prompt_name(char *output, size_t capacity,
                                        pxa_bytes_t value,
                                        const char *empty_value) {
    size_t size;
    if (output == NULL || capacity == 0) return;
    if (value.size == 0 || value.data == NULL) {
        copy_utf8_c_string(output, capacity, empty_value);
        return;
    }
    size = value.size < capacity - 1 ? value.size : capacity - 1;
    if (size < value.size) {
        while (size != 0 && (value.data[size] & 0xc0u) == 0x80u) --size;
    }
    if (size == 0) {
        copy_utf8_c_string(output, capacity, empty_value);
        return;
    }
    memcpy(output, value.data, size);
    output[size] = '\0';
}

static void dismiss_runtime_permission_prompt(void) {
    if (!g_host.activation.permission_prompt.active) return;
    pxa_esp_ui_shell_dismiss_permission_prompt(
        g_host.activation.permission_prompt.prompt_id);
    memset(&g_host.activation.permission_prompt, 0, sizeof(g_host.activation.permission_prompt));
    pxa_esp_surface_runtime_modal_leave();
}

static void dismiss_unresponsive_prompt(void) {
    uint32_t prompt_id;
    portENTER_CRITICAL(&g_wamr_watchdog_lock);
    prompt_id = g_host.unresponsive_prompt_id;
    g_host.unresponsive_prompt_id = 0;
    portEXIT_CRITICAL(&g_wamr_watchdog_lock);
    if (prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_unresponsive_prompt(prompt_id);
        pxa_esp_surface_runtime_modal_leave();
    }
}

static pxa_status_t request_runtime_permission_prompt(
    void *context, pxa_component_t component, uint32_t request_id,
    pxa_bytes_t app_identity, pxa_bytes_t name, pxa_bytes_t scope) {
    pxa_esp_ui_permission_prompt_t prompt;
    uint32_t prompt_id;
    (void)context;
    if ((g_host.activation.active_component != PXA_COMPONENT_INVALID &&
         component != g_host.activation.active_component) ||
        request_id == 0 ||
        app_identity.size != strlen(g_host.activation.active_identity) ||
        app_identity.size == 0 ||
        memcmp(app_identity.data, g_host.activation.active_identity, app_identity.size) != 0 ||
        (g_host.activation.permission_prompt.active ||
         (g_store_install != NULL && g_store_install->prompt_id != 0))) {
        return PXA_STATUS_BUSY;
    }
    prompt_id = ++g_host.activation.next_permission_prompt_id;
    if (prompt_id == 0) prompt_id = ++g_host.activation.next_permission_prompt_id;
    memset(&prompt, 0, sizeof(prompt));
    prompt.prompt_id = prompt_id;
    copy_utf8_c_string(
        prompt.app_name, sizeof(prompt.app_name),
        g_host.activation.active_name[0] == '\0'
            ? g_host.activation.active_identity
            : g_host.activation.active_name);
    copy_permission_prompt_text(prompt.permission_name,
                                sizeof(prompt.permission_name), name,
                                pxa_esp_host_locale_is_chinese() ?
                                    "未知权限" : "Unknown permission");
    copy_permission_prompt_text(prompt.scope, sizeof(prompt.scope), scope, "-");
    if (pxa_esp_host_locale_is_chinese()) {
        if (strcmp(prompt.permission_name, "net.client") == 0)
            snprintf(prompt.permission_name, sizeof(prompt.permission_name),
                     "网络访问 (net.client)");
        else if (strcmp(prompt.permission_name, "device.identity") == 0)
            snprintf(prompt.permission_name, sizeof(prompt.permission_name),
                     "设备标识 (device.identity)");
        else if (strcmp(prompt.permission_name, "audio.playback") == 0)
            snprintf(prompt.permission_name, sizeof(prompt.permission_name),
                     "音频播放");
        if (strcmp(prompt.scope, "media") == 0)
            snprintf(prompt.scope, sizeof(prompt.scope), "媒体音频");
    }
    g_host.activation.permission_prompt.active = 1;
    g_host.activation.permission_prompt.prompt_id = prompt_id;
    g_host.activation.permission_prompt.component = component;
    g_host.activation.permission_prompt.request_id = request_id;
    pxa_esp_surface_runtime_modal_enter();
    if (!pxa_esp_ui_shell_post_permission_prompt(&prompt)) {
        memset(&g_host.activation.permission_prompt, 0, sizeof(g_host.activation.permission_prompt));
        pxa_esp_surface_runtime_modal_leave();
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Runtime permission prompt queued: app=%s permission=%s",
             g_host.activation.active_identity, prompt.permission_name);
    return PXA_STATUS_OK;
}

bool pxa_esp_host_respond_permission(uint32_t prompt_id, bool granted) {
    pxa_esp_host_command_t command;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_PERMISSION_DECISION;
    command.payload.permission_decision.prompt_id = prompt_id;
    command.payload.permission_decision.granted = granted ? 1u : 0u;
    if (post_command(&command)) return true;
    ESP_LOGW(PXA_ESP_HOST_TAG, "Permission decision queue full: prompt=%u",
             (unsigned)prompt_id);
    return false;
}

bool pxa_esp_host_respond_store_result(uint32_t prompt_id, bool open) {
    return pxa_esp_host_respond_permission(prompt_id, open);
}

bool pxa_esp_host_respond_unresponsive(uint32_t prompt_id, bool wait) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    int handled;
    int current;
    int cleared = 0;
    portENTER_CRITICAL(&g_wamr_watchdog_lock);
    current = prompt_id != 0 && prompt_id == g_host.unresponsive_prompt_id;
    portEXIT_CRITICAL(&g_wamr_watchdog_lock);
    if (!current) return true;
    if (g_host.engine == NULL) {
        portENTER_CRITICAL(&g_wamr_watchdog_lock);
        if (g_host.unresponsive_prompt_id == prompt_id) {
            g_host.unresponsive_prompt_id = 0;
            cleared = 1;
        }
        portEXIT_CRITICAL(&g_wamr_watchdog_lock);
        if (cleared) pxa_esp_surface_runtime_modal_leave();
        return true;
    }
    public_state_snapshot(&public_state);
    if (wait) {
        handled = pxa_wamr_engine_extend_active_deadline(
            g_host.engine, host_now_us(NULL));
    } else {
        /* Terminate immediately when possible, then stop the package on the
         * runtime thread. The queued stop also handles a call that returned
         * before the user responded. */
        handled = pxa_wamr_engine_terminate_active_call(g_host.engine);
        memset(&command, 0, sizeof(command));
        command.type = (uint8_t)PXA_ESP_HOST_CMD_UNRESPONSIVE_STOP;
        snprintf(command.payload.identity.identity,
                 sizeof(command.payload.identity.identity), "%s",
                 public_state.identity);
        if (!post_command(&command)) {
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Unresponsive stop queue full: prompt=%u",
                     (unsigned)prompt_id);
            return false;
        }
    }
    portENTER_CRITICAL(&g_wamr_watchdog_lock);
    if (g_host.unresponsive_prompt_id == prompt_id) {
        g_host.unresponsive_prompt_id = 0;
        cleared = 1;
    }
    portEXIT_CRITICAL(&g_wamr_watchdog_lock);
    if (cleared) pxa_esp_surface_runtime_modal_leave();
    ESP_LOGW(PXA_ESP_HOST_TAG, "Guest callback timeout: app=%s action=%s%s",
             public_state.identity[0] == '\0' ? "<none>" :
                                                  public_state.identity,
             wait ? "wait" : "stop", handled ? "" : " (call already ended)");
    return true;
}

/* --- event delivery ---------------------------------------------------- */

static void stop_active(pxa_stop_reason_t reason);

static void fault_active_component(pxa_component_t component,
                                   pxa_status_t status) {
    ESP_LOGE(PXA_ESP_HOST_TAG,
             "Guest event failed: app=%s component=%u status=%d",
             g_host.activation.active_identity[0] == '\0' ? "<none>" :
                                                   g_host.activation.active_identity,
             (unsigned)component, (int)status);
    pxa_esp_ui_shell_post_toast("应用运行异常", 1800);
    stop_active(PXA_STOP_FAULT);
}

static bool event_delivery_deferred(
    pxa_status_t status, const pxa_wamr_event_result_t *result) {
    return status == PXA_STATUS_WOULD_BLOCK ||
           (status == PXA_STATUS_RESOURCE_LIMIT && result != NULL &&
            result->event_consumed == 0);
}

static int drain_events(pxa_component_t component) {
    pxa_wamr_event_result_t result;
    const pxa_status_t status = pxa_wamr_engine_deliver_event_result(
        g_host.engine, g_host.activation.runtime, component, &result);
    if (status == PXA_STATUS_OK) {
        (void)pxa_ipc_flush(g_host.activation.services.ipc);
        return 1;
    }
    if (!event_delivery_deferred(status, &result)) {
        fault_active_component(component, status);
    }
    return 0;
}

static uint32_t current_wamr_bytes(void) {
    pxa_wamr_memory_snapshot_t usage;
    return g_host.engine != NULL &&
                   pxa_wamr_engine_memory_snapshot(g_host.engine, &usage) ==
                       PXA_STATUS_OK
               ? usage.current_bytes
               : 0;
}

static void host_log_heap_usage(const char *stage) {
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_total = heap_caps_get_total_size(internal_caps);
    const size_t internal_free = heap_caps_get_free_size(internal_caps);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#ifdef MALLOC_CAP_EXEC
    const uint32_t executable_caps = MALLOC_CAP_EXEC;
#else
    /* ESP32-S31 executes AOT code from its unified executable PSRAM range. */
    const uint32_t executable_caps = MALLOC_CAP_SPIRAM;
#endif
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Heap[%s]: SRAM used=%u free=%u total=%u min_free=%u "
             "largest=%u exec_free=%u exec_largest=%u; "
             "PSRAM used=%u free=%u total=%u min_free=%u largest=%u",
             stage == NULL ? "-" : stage,
             (unsigned)(internal_total - internal_free),
             (unsigned)internal_free, (unsigned)internal_total,
             (unsigned)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned)heap_caps_get_largest_free_block(internal_caps),
             (unsigned)heap_caps_get_free_size(executable_caps),
             (unsigned)heap_caps_get_largest_free_block(executable_caps),
             (unsigned)(psram_total - psram_free), (unsigned)psram_free,
             (unsigned)psram_total,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static const pxa_package_service_capability_t *find_service_capability(
    const pxa_package_activation_profile_t *host, uint16_t service) {
    size_t index;
    for (index = 0; index < host->service_count; ++index) {
        if (host->services[index].service == service)
            return &host->services[index];
    }
    return NULL;
}

static const char *activation_service_name(uint16_t service_id) {
    switch (service_id) {
        case PXA_WINDOW_SERVICE_ID: return "Window";
        case PXA_UI_SERVICE_ID: return "UI";
        case PXA_DEVICE_SERVICE_ID: return "Device";
        case PXA_STORE_INSTALL_SERVICE_ID: return "Installer";
        default: return "Service";
    }
}

static void log_activation_compatibility(
    const pxa_package_manifest_t *manifest,
    const pxa_package_activation_profile_t *capabilities,
    const pxa_package_host_profile_t *host,
    char *reason, size_t reason_size) {
    uint16_t component_index;
    if (reason_size != 0) reason[0] = '\0';
    for (component_index = 0; component_index < manifest->component_count;
         ++component_index) {
        const pxa_package_component_t *component =
            &manifest->components[component_index];
        uint16_t requirement_index;
        for (requirement_index = 0;
             requirement_index < component->service_count;
             ++requirement_index) {
            const pxa_package_service_requirement_t *requirement =
                &component->services[requirement_index];
            const pxa_package_service_capability_t *capability =
                find_service_capability(capabilities, requirement->service);
            if (capability == NULL) {
                if (reason_size != 0 && reason[0] == '\0')
                    snprintf(reason, reason_size, "%s(%u) 服务不可用",
                             activation_service_name(requirement->service),
                             (unsigned)requirement->service);
                ESP_LOGE(PXA_ESP_HOST_TAG,
                         "Compatibility: component=%.*s service=%u is absent",
                         (int)component->id.size,
                         (const char *)component->id.data,
                         (unsigned)requirement->service);
                continue;
            }
            if (capability->version.major != requirement->min_version.major ||
                capability->version.major != requirement->max_version.major ||
                capability->version.minor < requirement->min_version.minor ||
                capability->version.minor > requirement->max_version.minor ||
                (requirement->required_features & ~capability->features) != 0) {
                if (reason_size != 0 && reason[0] == '\0') {
                    if (capability->version.major != requirement->min_version.major ||
                        capability->version.minor < requirement->min_version.minor ||
                        capability->version.minor > requirement->max_version.minor) {
                        snprintf(reason, reason_size,
                                 "%s(%u) 需要 %u.%u，设备为 %u.%u",
                                 activation_service_name(requirement->service),
                                 (unsigned)requirement->service,
                                 (unsigned)requirement->min_version.major,
                                 (unsigned)requirement->min_version.minor,
                                 (unsigned)capability->version.major,
                                 (unsigned)capability->version.minor);
                    } else {
                        snprintf(reason, reason_size,
                                 "%s(%u) 缺少所需能力",
                                 activation_service_name(requirement->service),
                                 (unsigned)requirement->service);
                    }
                }
                ESP_LOGE(PXA_ESP_HOST_TAG,
                         "Compatibility: component=%.*s service=%u "
                         "requires=%u.%u-%u.%u features=0x%08x%08x "
                         "host=%u.%u features=0x%08x%08x",
                         (int)component->id.size,
                         (const char *)component->id.data,
                         (unsigned)requirement->service,
                         (unsigned)requirement->min_version.major,
                         (unsigned)requirement->min_version.minor,
                         (unsigned)requirement->max_version.major,
                         (unsigned)requirement->max_version.minor,
                         (unsigned)(requirement->required_features >> 32),
                         (unsigned)requirement->required_features,
                         (unsigned)capability->version.major,
                         (unsigned)capability->version.minor,
                         (unsigned)(capability->features >> 32),
                         (unsigned)capability->features);
            }
        }
        {
            const pxa_package_artifact_t *artifact = NULL;
            if (pxa_package_artifact_select(component, host, &artifact) !=
                PXA_STATUS_OK) {
                if (reason_size != 0 && reason[0] == '\0')
                    snprintf(reason, reason_size, "没有适用于 %.*s 的安装包",
                             (int)host->target.size, (const char *)host->target.data);
                ESP_LOGE(PXA_ESP_HOST_TAG,
                         "Compatibility: component=%.*s has no artifact for "
                         "target=%.*s engine=%.*s abi=%.*s",
                         (int)component->id.size,
                         (const char *)component->id.data,
                         (int)host->target.size,
                         (const char *)host->target.data,
                         (int)host->engine.size,
                         (const char *)host->engine.data,
                         (int)host->engine_abi.size,
                         (const char *)host->engine_abi.data);
            }
        }
    }
}

static pxa_status_t activate_component_profiled(
    pxa_bytes_t component_id, uint64_t instance_id,
    pxa_component_t *component_output) {
    const size_t internal_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_before =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t wamr_before = current_wamr_bytes();
    const pxa_status_t status = pxa_activation_activate(
        g_host.activation.coordinator, component_id, instance_id,
        component_output);
    const size_t internal_after =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const uint32_t wamr_after = current_wamr_bytes();
    const int psram_delta =
        psram_after <= psram_before ? (int)(psram_before - psram_after)
                                    : -(int)(psram_after - psram_before);
    const int internal_delta =
        internal_after <= internal_before
            ? (int)(internal_before - internal_after)
            : -(int)(internal_after - internal_before);
    const int wamr_delta =
        wamr_after >= wamr_before ? (int)(wamr_after - wamr_before)
                                  : -(int)(wamr_before - wamr_after);
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Component memory: id=%.*s status=%d sram_delta=%d "
             "free_sram=%u largest_sram=%u psram_delta=%d free_psram=%u "
             "largest_psram=%u wamr_delta=%d wamr_current=%u",
             (int)component_id.size, (const char *)component_id.data,
             (int)status, internal_delta, (unsigned)internal_after,
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             psram_delta, (unsigned)psram_after,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             wamr_delta, (unsigned)wamr_after);
    return status;
}

static int track_active_component(pxa_component_t component) {
    size_t index;
    if (component == PXA_COMPONENT_INVALID) return 0;
    for (index = 0; index < g_host.activation.active_component_count; ++index) {
        if (g_host.activation.active_components[index] == component) return 1;
    }
    if (g_host.activation.active_component_count >=
        g_host.activation.active_component_capacity) {
        return 0;
    }
    g_host.activation.active_components[g_host.activation.active_component_count++] = component;
    return 1;
}

static void untrack_active_component(pxa_component_t component) {
    size_t index;
    for (index = 0; index < g_host.activation.active_component_count; ++index) {
        if (g_host.activation.active_components[index] == component) {
            memmove(&g_host.activation.active_components[index],
                    &g_host.activation.active_components[index + 1u],
                    (size_t)(g_host.activation.active_component_count - index - 1u) *
                        sizeof(g_host.activation.active_components[0]));
            --g_host.activation.active_component_count;
            return;
        }
    }
}

static void clear_component_timer_slots(pxa_component_t component);

static pxa_status_t resolve_ipc_endpoint(void *context,
                                         pxa_bytes_t endpoint_name,
                                         pxa_component_t *provider_output) {
    const pxa_package_ipc_endpoint_t *endpoint = NULL;
    pxa_component_t provider = PXA_COMPONENT_INVALID;
    uint64_t instance_id = 0;
    pxa_status_t status;
    uint16_t index;
    int activated = 0;
    (void)context;
    if (provider_output == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    *provider_output = PXA_COMPONENT_INVALID;
    if (g_host.activation.active_manifest == NULL ||
        g_host.activation.coordinator == NULL) {
        return PXA_STATUS_BAD_STATE;
    }
    for (index = 0;
         index < g_host.activation.active_manifest->ipc_endpoint_count;
         ++index) {
        const pxa_package_ipc_endpoint_t *candidate =
            &g_host.activation.active_manifest->ipc_endpoints[index];
        if (candidate->name.size == endpoint_name.size &&
            memcmp(candidate->name.data, endpoint_name.data,
                   endpoint_name.size) == 0) {
            endpoint = candidate;
            break;
        }
    }
    if (endpoint == NULL) return PXA_STATUS_NOT_FOUND;
    status = pxa_activation_find(g_host.activation.coordinator,
                                 endpoint->component_id, &instance_id,
                                 &provider);
    if (status == PXA_STATUS_NOT_FOUND) {
        instance_id = allocate_instance_id();
        status = activate_component_profiled(endpoint->component_id,
                                              instance_id, &provider);
        activated = status == PXA_STATUS_OK;
    }
    if (status == PXA_STATUS_OK && !track_active_component(provider)) {
        if (activated) {
            (void)pxa_activation_deactivate(
                g_host.activation.coordinator, endpoint->component_id,
                PXA_STOP_FAULT);
            clear_component_timer_slots(provider);
        }
        status = PXA_STATUS_RESOURCE_LIMIT;
    }
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "IPC endpoint activation failed: endpoint=%.*s "
                 "component=%.*s status=%d",
                 (int)endpoint_name.size, (const char *)endpoint_name.data,
                 (int)endpoint->component_id.size,
                 (const char *)endpoint->component_id.data, (int)status);
        return status;
    }
    *provider_output = provider;
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "IPC endpoint ready: endpoint=%.*s component=%.*s "
             "instance=%08x%08x",
             (int)endpoint_name.size, (const char *)endpoint_name.data,
             (int)endpoint->component_id.size,
             (const char *)endpoint->component_id.data,
             (unsigned)(instance_id >> 32), (unsigned)instance_id);
    return PXA_STATUS_OK;
}

static void clear_component_timer_slots(pxa_component_t component) {
    portENTER_CRITICAL(&g_clock_slots_lock);
    pxa_host_clock_slots_clear_component(&g_host.clock_slots, component);
    portEXIT_CRITICAL(&g_clock_slots_lock);
}

static int allocate_job_slots(uint16_t count) {
    if (count == 0) return 1;
    g_host.activation.active_jobs = activation_alloc(
        (size_t)count * sizeof(g_host.activation.active_jobs[0]));
    g_host.activation.job_stop_at_ms = activation_alloc(
        (size_t)count * sizeof(g_host.activation.job_stop_at_ms[0]));
    g_host.activation.job_force_stop_at_ms = activation_alloc(
        (size_t)count * sizeof(g_host.activation.job_force_stop_at_ms[0]));
    g_host.activation.active_work = activation_alloc(
        (size_t)count * sizeof(g_host.activation.active_work[0]));
    g_host.activation.work_result = activation_alloc(
        (size_t)count * sizeof(g_host.activation.work_result[0]));
    if (g_host.activation.active_jobs == NULL ||
        g_host.activation.job_stop_at_ms == NULL ||
        g_host.activation.job_force_stop_at_ms == NULL ||
        g_host.activation.active_work == NULL ||
        g_host.activation.work_result == NULL) {
        return 0;
    }
    memset(g_host.activation.active_jobs, 0,
           (size_t)count * sizeof(g_host.activation.active_jobs[0]));
    memset(g_host.activation.job_stop_at_ms, 0,
           (size_t)count * sizeof(g_host.activation.job_stop_at_ms[0]));
    memset(g_host.activation.job_force_stop_at_ms, 0,
           (size_t)count * sizeof(g_host.activation.job_force_stop_at_ms[0]));
    memset(g_host.activation.active_work, 0,
           (size_t)count * sizeof(g_host.activation.active_work[0]));
    memset(g_host.activation.work_result, 0,
           (size_t)count * sizeof(g_host.activation.work_result[0]));
    return 1;
}

static int job_component_slot(pxa_bytes_t component_id) {
    uint16_t index;
    for (index = 0; index < g_host.activation.services.job_component_count; ++index) {
        size_t name_size = strlen(g_host.activation.services.job_components[index]);
        if (name_size == component_id.size &&
            memcmp(g_host.activation.services.job_components[index], component_id.data,
                   component_id.size) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int has_active_jobs(void) {
    uint16_t index;
    for (index = 0; index < g_host.activation.services.job_component_count; ++index) {
        if (g_host.activation.active_jobs[index] != PXA_COMPONENT_INVALID) return 1;
    }
    return 0;
}

static pxa_status_t complete_work(void *context, pxa_component_t component,
                                  uint32_t work_id,
                                  pxa_work_result_t result) {
    uint16_t index;
    (void)context;
    for (index = 0;
         index < g_host.activation.services.job_component_count; ++index) {
        if (g_host.activation.active_jobs[index] == component &&
            g_host.activation.active_work[index].id == work_id &&
            g_host.activation.work_result[index] == 0) {
            g_host.activation.work_result[index] = result;
            return PXA_STATUS_OK;
        }
    }
    if (g_host.activation.activating_job_slot >= 0 &&
        (uint16_t)g_host.activation.activating_job_slot <
            g_host.activation.services.job_component_count &&
        g_host.activation
                .active_work[g_host.activation.activating_job_slot].id ==
            work_id &&
        g_host.activation.work_result[
            g_host.activation.activating_job_slot] == 0) {
        g_host.activation.work_result[
            g_host.activation.activating_job_slot] = result;
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_NOT_FOUND;
}

static void defer_work(const pxa_scheduler_entry_t *entry) {
    (void)pxa_scheduler_defer(g_host.activation.services.scheduler, entry,
                              PXA_SCHEDULER_DEFAULT_MIN_DELAY_MS);
}

static pxa_status_t cancel_work(void *context, uint32_t work_id) {
    uint16_t index;
    (void)context;
    for (index = 0;
         index < g_host.activation.services.job_component_count; ++index) {
        if (g_host.activation.active_jobs[index] != PXA_COMPONENT_INVALID &&
            g_host.activation.active_work[index].id == work_id &&
            g_host.activation.work_result[index] == 0) {
            g_host.activation.work_result[index] = PXA_WORK_RESULT_FAILURE;
            return PXA_STATUS_OK;
        }
    }
    return PXA_STATUS_NOT_FOUND;
}

static void finish_job(uint16_t index, pxa_work_result_t result) {
    const pxa_component_t component = g_host.activation.active_jobs[index];
    const pxa_scheduler_entry_t entry =
        g_host.activation.active_work[index];
    if (component == PXA_COMPONENT_INVALID) return;
    if (result == PXA_WORK_RESULT_RETRY &&
        entry.attempt < entry.max_attempts) {
        (void)pxa_scheduler_retry(g_host.activation.services.scheduler,
                                  &entry);
    }
    (void)pxa_activation_deactivate(
        g_host.activation.coordinator,
        (pxa_bytes_t){
            (const uint8_t *)g_host.activation.services.job_components[index],
            strlen(g_host.activation.services.job_components[index])},
        PXA_STOP_NORMAL);
    clear_component_timer_slots(component);
    untrack_active_component(component);
    g_host.activation.active_jobs[index] = PXA_COMPONENT_INVALID;
    g_host.activation.job_stop_at_ms[index] = 0;
    g_host.activation.job_force_stop_at_ms[index] = 0;
    memset(&g_host.activation.active_work[index], 0,
           sizeof(g_host.activation.active_work[index]));
    g_host.activation.work_result[index] = 0;
}

static void maintain_jobs(void) {
    const uint64_t now_ms = host_clock_ms(NULL);
    uint16_t index;
    for (index = 0; index < g_host.activation.services.job_component_count; ++index) {
        const pxa_component_t component = g_host.activation.active_jobs[index];
        const pxa_scheduler_entry_t *entry =
            &g_host.activation.active_work[index];
        if (component == PXA_COMPONENT_INVALID) continue;
        if (g_host.activation.work_result[index] != 0) {
            finish_job(index, g_host.activation.work_result[index]);
        } else if (now_ms >= g_host.activation.job_stop_at_ms[index]) {
            if (g_host.activation.job_force_stop_at_ms[index] == 0) {
                if (pxa_scheduler_post_work_stop(
                        g_host.activation.services.scheduler, component,
                        entry->id,
                        g_host.activation.job_stop_at_ms[index]) ==
                    PXA_STATUS_OK) {
                    g_host.activation.job_force_stop_at_ms[index] =
                        now_ms + PXA_ESP_HOST_WORK_CANCEL_GRACE_MS;
                } else {
                    finish_job(index, PXA_WORK_RESULT_RETRY);
                }
            } else if (now_ms >=
                       g_host.activation.job_force_stop_at_ms[index]) {
                finish_job(index, PXA_WORK_RESULT_RETRY);
            }
        }
    }
}

static void refresh_volume_key_capture_state(void) {
    uint8_t captures = 0;
    if (g_host.activation.services.ui != NULL &&
        g_host.activation.ui_component != PXA_COMPONENT_INVALID &&
        g_host.activation.active_component != PXA_COMPONENT_INVALID) {
        captures = pxa_ui_accepts_event(
            g_host.activation.services.ui, g_host.activation.ui_component,
            PXA_UI_PRIMARY_SURFACE, 1u, PXA_UI_EVENT_KEY)
                       ? 1u
                       : 0u;
    }
    portENTER_CRITICAL(&g_public_state_lock);
    if (g_host.public_state.main_active) {
        g_host.public_state.volume_key_capture_active = captures;
    }
    portEXIT_CRITICAL(&g_public_state_lock);
}

static int drain_active_events(void) {
    uint8_t round;
    for (round = 0; round < PXA_ESP_HOST_EVENT_DRAIN_ROUNDS; ++round) {
        size_t index;
        int delivered = 0;
        for (index = 0; index < g_host.activation.active_component_count; ++index) {
            pxa_wamr_event_result_t result;
            const pxa_status_t status = pxa_wamr_engine_deliver_event_result(
                g_host.engine, g_host.activation.runtime,
                g_host.activation.active_components[index], &result);
            if (status == PXA_STATUS_OK) {
                delivered = 1;
                (void)pxa_ipc_flush(g_host.activation.services.ipc);
            }
            else if (!event_delivery_deferred(status, &result)) {
                fault_active_component(g_host.activation.active_components[index], status);
                return 0;
            }
        }
        if (!delivered) break;
    }
    refresh_volume_key_capture_state();
    return 1;
}

static void mark_net_completion_ready(void) {
    portENTER_CRITICAL(&g_net_completion_lock);
    g_net_completion_ready = 1;
    portEXIT_CRITICAL(&g_net_completion_lock);
    wake_runtime_thread();
}

static void on_net_completion_ready(void *context) {
    (void)context;
    mark_net_completion_ready();
    if (!post_maintenance(PXA_ESP_HOST_CMD_NET_MAINTENANCE)) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Network completion maintenance queue is full");
    }
}

static int take_net_completion_ready(void) {
    int ready;
    portENTER_CRITICAL(&g_net_completion_lock);
    ready = g_net_completion_ready != 0;
    g_net_completion_ready = 0;
    portEXIT_CRITICAL(&g_net_completion_lock);
    return ready;
}

static void drain_net_completions(void) {
    pxa_component_t affected[PXA_ESP_NET_MAX_PENDING];
    size_t count = 0;
    pxa_status_t status;
    if (g_host.activation.services.net == NULL) return;
    status = pxa_net_poll(g_host.activation.services.net, affected,
                          PXA_ESP_NET_MAX_PENDING, &count);
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Network completion poll failed: status=%d", (int)status);
    } else if (count != 0) {
        ESP_LOGI(PXA_ESP_HOST_TAG,
                 "Network completion delivered: components=%u",
                 (unsigned)count);
        drain_active_events();
    }
}

/* --- synchronous LVGL executor ----------------------------------------- */

static pxa_status_t esp_lvgl_execute(
    pxa_lvgl_ui_execute_callback_fn callback, void *callback_data,
    void *user_data) {
    (void)user_data;
    lv_lock();
    callback(callback_data);
    lv_unlock();
    pxa_esp_surface_require_composition();
    return PXA_STATUS_OK;
}

static bool provide_ui_alpha_plane(
    void *context, pxa_esp_surface_ui_alpha_plane_t *output) {
    pxa_lvgl_ui_alpha_plane_t plane;
    pxa_esp_host_t *host = (pxa_esp_host_t *)context;
    if (output == NULL || host == NULL || host->ui_adapter == NULL)
        return false;
    memset(output, 0, sizeof(*output));
    if (!pxa_lvgl_ui_alpha_plane(host->ui_adapter, &plane)) return true;
    output->pixels = plane.pixels;
    output->alpha = plane.alpha;
    output->pixel_stride_bytes = plane.pixel_stride_bytes;
    output->alpha_stride_bytes = plane.alpha_stride_bytes;
    output->x = plane.x;
    output->y = plane.y;
    output->width = plane.width;
    output->height = plane.height;
    output->opacity = 255;
    output->visible = 1;
    output->revision = plane.revision;
    return true;
}

/* --- pointer ------------------------------------------------------------ */

/* The Canvas callback runs on the LVGL task while guest callbacks run on the
 * PXA owner task. Keep edge events in order, but collapse only contiguous
 * movement when the guest cannot keep up with the sampling rate. */
static int enqueue_pointer_event(const pxa_host_pointer_event_t *event) {
    int queued;
    portENTER_CRITICAL(&g_pointer_mailbox_lock);
    queued = pxa_host_pointer_mailbox_push(
        &g_host.pointer_mailbox, event);
    portEXIT_CRITICAL(&g_pointer_mailbox_lock);
    return queued;
}

static int take_pointer_command(pxa_esp_host_command_t *command,
                                uint64_t now_us) {
    pxa_host_pointer_event_t event;
    int available;

    portENTER_CRITICAL(&g_pointer_mailbox_lock);
    available = pxa_host_pointer_mailbox_take(
        &g_host.pointer_mailbox, now_us,
        PXA_ESP_HOST_POINTER_MOVE_MIN_INTERVAL_US, &event);
    portEXIT_CRITICAL(&g_pointer_mailbox_lock);
    if (available) {
        memset(command, 0, sizeof(*command));
        command->type = (uint8_t)PXA_ESP_HOST_CMD_POINTER;
        command->payload.pointer = event;
    }
    return available;
}

static int pointer_mailbox_has_move(void) {
    int pending;

    portENTER_CRITICAL(&g_pointer_mailbox_lock);
    pending = pxa_host_pointer_mailbox_has_move(
        &g_host.pointer_mailbox);
    portEXIT_CRITICAL(&g_pointer_mailbox_lock);
    return pending;
}

static int pointer_move_is_due(uint64_t now_us) {
    int due;

    portENTER_CRITICAL(&g_pointer_mailbox_lock);
    due = pxa_host_pointer_mailbox_move_is_due(
        &g_host.pointer_mailbox, now_us,
        PXA_ESP_HOST_POINTER_MOVE_MIN_INTERVAL_US);
    portEXIT_CRITICAL(&g_pointer_mailbox_lock);
    return due;
}

static void clear_pointer_mailbox(void) {
    portENTER_CRITICAL(&g_pointer_mailbox_lock);
    pxa_host_pointer_mailbox_init(&g_host.pointer_mailbox);
    portEXIT_CRITICAL(&g_pointer_mailbox_lock);
}

static void queue_ui_pointer(uint32_t surface, uint32_t node,
                             const uint8_t *value, size_t value_size) {
    pxa_host_pointer_event_t event;
    pxa_esp_host_public_state_t public_state;
    uint64_t timestamp_us;
    uint8_t phase;
    if (value == NULL || value_size != 12) return;
    public_state_snapshot(&public_state);
    if (!public_state.main_active || public_state.main_instance_id == 0) return;
    phase = value[1];
    memset(&event, 0, sizeof(event));
    event.surface = surface;
    event.node = node;
    event.id = value[0];
    event.phase = phase;
    event.x = (int32_t)pxa_read_u32(value + 4);
    event.y = (int32_t)pxa_read_u32(value + 8);
    event.instance_id = public_state.main_instance_id;
    timestamp_us = pxa_lvgl_ui_event_timestamp_us(g_host.ui_adapter);
    event.timestamp_us = timestamp_us != 0 ? timestamp_us : host_now_us(NULL);
    if (phase != PXA_HOST_POINTER_MOVE_PHASE) {
        ESP_LOGI(PXA_ESP_HOST_TAG, "pointer queued id=%u phase=%u x=%d y=%d",
                 (unsigned)event.id, (unsigned)event.phase, (int)event.x,
                 (int)event.y);
    }
    if (enqueue_pointer_event(&event) &&
        (phase != PXA_HOST_POINTER_MOVE_PHASE ||
         pointer_move_is_due(event.timestamp_us))) {
        wake_runtime_thread();
    }
}

static void on_ui_event(uint32_t surface, uint32_t node,
                        pxa_ui_event_kind_t kind, uint16_t flags,
                        const void *value, size_t value_size,
                        void *user_data) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    uint64_t timestamp_us;
    (void)user_data;
    if (kind == PXA_UI_EVENT_POINTER) {
        queue_ui_pointer(surface, node, (const uint8_t *)value, value_size);
        return;
    }
    public_state_snapshot(&public_state);
    if (!public_state.main_active || public_state.main_instance_id == 0) return;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_UI_EVENT;
    command.payload.ui_event.surface = surface;
    command.payload.ui_event.node = node;
    command.payload.ui_event.kind = kind;
    command.payload.ui_event.flags = flags;
    command.payload.ui_event.instance_id = public_state.main_instance_id;
    command.payload.ui_event.value =
        value_size >= 4 ? (int32_t)pxa_read_u32(value) : 0;
    if (kind == PXA_UI_EVENT_TEXT && value != NULL && value_size != 0) {
        size_t copy = value_size < PXA_HOST_UI_EVENT_TEXT_BYTES
                          ? value_size
                          : PXA_HOST_UI_EVENT_TEXT_BYTES;
        memcpy(command.payload.ui_event.text, value, copy);
        command.payload.ui_event.text_size = (uint16_t)copy;
    }
    timestamp_us = pxa_lvgl_ui_event_timestamp_us(g_host.ui_adapter);
    command.payload.ui_event.timestamp_us =
        timestamp_us != 0 ? timestamp_us : host_now_us(NULL);
    if (!post_command(&command)) {
        ESP_LOGE(PXA_ESP_HOST_TAG, "Reliable UI event queue is full");
    }
}

/* --- window backend ----------------------------------------------------- */

static pxa_status_t host_window_apply(
    void *context, const pxa_window_configuration_t *configuration) {
    pxa_host_window_changed_fn callback;
    void *callback_context;
    (void)context;
    if (configuration == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    portENTER_CRITICAL(&g_window_changed_lock);
    callback = g_window_changed_callback;
    callback_context = g_window_changed_context;
    portEXIT_CRITICAL(&g_window_changed_lock);
    if (callback != NULL)
        callback(callback_context, g_host.activation.active_identity, configuration);
    return PXA_STATUS_OK;
}

static pxa_status_t host_window_toast(void *context, const char *text,
                                      uint32_t duration_ms) {
    (void)context;
    pxa_esp_ui_shell_post_toast(text, duration_ms);
    return PXA_STATUS_OK;
}

static int host_window_snapshot(pxa_window_snapshot_t *snapshot) {
    lv_display_t *display;
    int32_t logical_width;
    int32_t logical_height;
    int32_t pixel_width;
    int32_t pixel_height;

    if (snapshot == NULL) return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    /* The PXA runtime is separate from the LVGL owner task. Reading display
     * state under LVGL's lock makes rotation and resolution changes atomic. */
    lv_lock();
    display = lv_display_get_default();
    if (display == NULL) {
        lv_unlock();
        return 0;
    }
    logical_width = lv_display_get_horizontal_resolution(display);
    logical_height = lv_display_get_vertical_resolution(display);
    pixel_width = lv_display_get_physical_horizontal_resolution(display);
    pixel_height = lv_display_get_physical_vertical_resolution(display);
    lv_unlock();
    if (logical_width <= 0 || logical_height <= 0) return 0;

    snapshot->logical_width = (uint32_t)logical_width;
    snapshot->logical_height = (uint32_t)logical_height;
    snapshot->pixel_width = pixel_width > 0 ? (uint32_t)pixel_width
                                            : snapshot->logical_width;
    snapshot->pixel_height = pixel_height > 0 ? (uint32_t)pixel_height
                                              : snapshot->logical_height;
    /* Canvas commands and pointer events both use LVGL logical pixels. */
    snapshot->density_numerator = 1;
    snapshot->density_denominator = 1;
    if (logical_width > logical_height) {
        snapshot->orientation = PXA_WINDOW_ORIENTATION_LANDSCAPE;
    } else if (logical_height > logical_width) {
        snapshot->orientation = PXA_WINDOW_ORIENTATION_PORTRAIT;
    }
    snapshot->focused = 1;
    portENTER_CRITICAL(&g_process_state_lock);
    if (g_host.window_insets_valid) {
        snapshot->safe_insets = g_host.safe_insets;
        snapshot->system_bar_insets = g_host.system_bar_insets;
    }
    portEXIT_CRITICAL(&g_process_state_lock);
    return 1;
}

static void update_window_snapshot(pxa_component_t component) {
    pxa_window_snapshot_t snapshot;
    pxa_status_t status;
    if (g_host.activation.services.window == NULL || component == PXA_COMPONENT_INVALID ||
        !host_window_snapshot(&snapshot)) {
        return;
    }
    status = pxa_window_update_snapshot(g_host.activation.services.window, component, &snapshot);
    if (status != PXA_STATUS_OK && status != PXA_STATUS_NOT_FOUND) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Window metrics update failed: component=%u status=%d",
                 (unsigned)component, (int)status);
    }
}

/* --- clock / timer slots ------------------------------------------------ */

#if PXA_ESP_ACCEPTANCE_PERF
#define PXA_GUEST_PERF_FRAME_START UINT32_C(0x70)
#define PXA_GUEST_PERF_UPDATE_END UINT32_C(0x71)
#define PXA_GUEST_PERF_RAYCAST_START UINT32_C(0x72)
#define PXA_GUEST_PERF_RAYCAST_END UINT32_C(0x73)
#define PXA_GUEST_PERF_FRAME_END UINT32_C(0x74)

typedef struct {
    pxa_component_t component;
    uint64_t frame_start_us;
    uint64_t update_end_us;
    uint64_t raycast_start_us;
    uint64_t raycast_end_us;
} pxa_guest_perf_trace_t;

static pxa_guest_perf_trace_t g_guest_perf_trace;

static void trace_guest_perf_marker(pxa_component_t component,
                                    uint32_t request_id, uint64_t now_us) {
    if (request_id == PXA_GUEST_PERF_FRAME_START) {
        g_guest_perf_trace = (pxa_guest_perf_trace_t){
            .component = component,
            .frame_start_us = now_us,
        };
        return;
    }
    if (component != g_guest_perf_trace.component ||
        g_guest_perf_trace.frame_start_us == 0) {
        return;
    }
    switch (request_id) {
        case PXA_GUEST_PERF_UPDATE_END:
            g_guest_perf_trace.update_end_us = now_us;
            break;
        case PXA_GUEST_PERF_RAYCAST_START:
            g_guest_perf_trace.raycast_start_us = now_us;
            break;
        case PXA_GUEST_PERF_RAYCAST_END:
            g_guest_perf_trace.raycast_end_us = now_us;
            break;
        case PXA_GUEST_PERF_FRAME_END:
            if (g_guest_perf_trace.frame_start_us <=
                    g_guest_perf_trace.update_end_us &&
                g_guest_perf_trace.update_end_us <=
                    g_guest_perf_trace.raycast_start_us &&
                g_guest_perf_trace.raycast_start_us <
                    g_guest_perf_trace.raycast_end_us &&
                g_guest_perf_trace.raycast_end_us <= now_us) {
                ESP_LOGI("PxaGuestPerf",
                         "component=%u update_us=%llu raycast_us=%llu "
                         "total_us=%llu",
                         (unsigned)component,
                         (unsigned long long)(g_guest_perf_trace.update_end_us -
                                              g_guest_perf_trace.frame_start_us),
                         (unsigned long long)(g_guest_perf_trace.raycast_end_us -
                                              g_guest_perf_trace.raycast_start_us),
                         (unsigned long long)(now_us -
                                              g_guest_perf_trace.frame_start_us));
            }
            g_guest_perf_trace.frame_start_us = 0;
            break;
        default:
            break;
    }
}
#endif

static pxa_status_t host_set_timer(pxa_component_t component,
                                  uint16_t period_ms) {
    uint8_t slot;
    int configured;
    if (period_ms != 0 && (period_ms < 16 || period_ms > 1000)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    slot = (uint8_t)(component % PXA_HOST_CLOCK_SLOT_COUNT);
    portENTER_CRITICAL(&g_clock_slots_lock);
    configured = pxa_host_clock_slots_set(&g_host.clock_slots, slot,
                                           component, period_ms,
                                           host_now_us(NULL));
    portEXIT_CRITICAL(&g_clock_slots_lock);
    return configured ? PXA_STATUS_OK : PXA_STATUS_INVALID_ARGUMENT;
}

static pxa_status_t host_clock_control(void *context, pxa_runtime_t *runtime,
                                       pxa_component_t component,
                                       const pxa_message_view_t *message) {
    uint8_t response[24];
    uint8_t payload[12];
    pxa_writer_t writer;
    uint16_t period_ms;
    uint64_t now_us;
    (void)context;
    if (message->opcode == PXA_CLOCK_NOW) {
        if (message->request_id == 0 || message->payload.size != 0) {
            return PXA_STATUS_INVALID_ARGUMENT;
        }
        now_us = host_now_us(NULL);
#if PXA_ESP_ACCEPTANCE_PERF
        trace_guest_perf_marker(component, message->request_id, now_us);
#endif
        pxa_write_u32(payload, PXA_STATUS_OK);
        pxa_write_u64(payload + 4, now_us);
        pxa_writer_init(&writer, response, sizeof(response));
        if (pxa_writer_message(&writer, PXA_CLOCK_SERVICE_ID,
                               PXA_CLOCK_NOW_RESULT, message->request_id,
                               payload, sizeof(payload)) != PXA_STATUS_OK) {
            return PXA_STATUS_INTERNAL;
        }
        return pxa_event_post(
            runtime, component, response, writer.size, 0,
            ((uint64_t)PXA_CLOCK_SERVICE_ID << 48) |
                ((uint64_t)PXA_CLOCK_NOW_RESULT << 32) |
                message->request_id);
    }
    if (message->opcode != PXA_CLOCK_SET_PERIOD || message->request_id != 0 ||
        message->payload.size != 2) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    period_ms = pxa_read_u16(message->payload.data);
    return host_set_timer(component, period_ms);
}

static void host_runtime_limits_init(pxa_runtime_limits_t *limits,
                                     uint16_t max_components) {
    pxa_runtime_limits_init(limits);
    limits->max_components = max_components;
    limits->max_services = PXA_ESP_HOST_MAX_SERVICES;
}

static void host_log_runtime_usage(void) {
    pxa_runtime_usage_t usage;
    if (g_host.activation.runtime == NULL ||
        pxa_runtime_usage_snapshot(g_host.activation.runtime, &usage) != PXA_STATUS_OK)
        return;
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Runtime peaks: components=%u requests=%u handles=%u "
             "events=%u event_blocks=%u services=%u",
             (unsigned)usage.peak_components,
             (unsigned)usage.peak_requests,
             (unsigned)usage.peak_handles,
             (unsigned)usage.peak_events,
             (unsigned)usage.peak_event_blocks,
             (unsigned)usage.registered_services);
}

static void host_log_ui_usage(void) {
    pxa_ui_memory_snapshot_t usage;
    pxa_esp_ui_asset_snapshot_t assets;
    if (g_host.activation.services.ui == NULL || g_host.activation.ui_component == PXA_COMPONENT_INVALID ||
        pxa_ui_memory_snapshot(g_host.activation.services.ui, g_host.activation.ui_component, &usage) !=
            PXA_STATUS_OK)
        return;
    pxa_esp_ui_assets_snapshot(&assets);
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "UI peaks: dynamic=%u current=%u assets=%u current_assets=%u "
             "largest_decoded=%u "
             "asset_count=%u referenced=%u hits=%u misses=%u "
             "evictions=%u nodes=%u canvases=%u surfaces=%u",
             (unsigned)usage.peak_bytes,
             (unsigned)usage.current_bytes,
             (unsigned)assets.peak_bytes,
             (unsigned)assets.current_bytes,
             (unsigned)assets.largest_decoded_bytes,
             (unsigned)assets.asset_count,
             (unsigned)assets.referenced_assets,
             (unsigned)assets.cache_hits,
             (unsigned)assets.cache_misses,
             (unsigned)assets.evictions,
             (unsigned)usage.node_count,
             (unsigned)usage.canvas_count,
             (unsigned)usage.surface_count);
}

static void host_log_wamr_usage(void) {
    pxa_wamr_memory_snapshot_t usage;
    if (g_host.engine == NULL ||
        pxa_wamr_engine_memory_snapshot(g_host.engine, &usage) !=
            PXA_STATUS_OK)
        return;
    ESP_LOGI(PXA_ESP_HOST_TAG, "WAMR dynamic: peak=%u current=%u",
             (unsigned)usage.peak_bytes, (unsigned)usage.current_bytes);
}

static void host_log_audio_usage(void) {
    pxa_esp_audio_snapshot_t usage;
    pxa_esp_audio_snapshot(&usage);
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Audio: active=%u queued=%u/%u peak_queued=%u submitted=%u "
             "rendered=%u tones=%u/%u tone_drop=%u voice_exhaust=%u "
             "queue_full=%u stale=%u rejected=%u "
             "assets=%u/%u asset_fail=%u storage=%u stack=%u",
             (unsigned)usage.active_sessions, (unsigned)usage.queued_frames,
             (unsigned)usage.queue_capacity,
             (unsigned)usage.peak_queued_frames,
             (unsigned)usage.submitted_frames,
             (unsigned)usage.rendered_frames,
             (unsigned)usage.tone_commands,
             (unsigned)usage.tone_frames,
             (unsigned)usage.tone_dropped_commands,
             (unsigned)usage.voice_exhaustions,
             (unsigned)usage.queue_full_frames,
             (unsigned)usage.stale_frames,
             (unsigned)usage.sink_rejected_frames,
             (unsigned)usage.asset_play_commands,
             (unsigned)usage.asset_control_commands,
             (unsigned)usage.asset_command_failures,
             (unsigned)usage.queue_storage_bytes,
             (unsigned)usage.task_stack_bytes);
}

static void host_log_net_usage(void) {
    pxa_esp_net_snapshot_t usage;
    pxa_esp_net_snapshot(&usage);
    if (usage.slot_storage_bytes == 0) return;
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Network: active=%u peak_active=%u started=%u completed=%u "
             "cancelled=%u timed_out=%u queue_full=%u bytes=%u "
             "peak_response=%u slot_storage=%u",
             (unsigned)usage.active_slots,
             (unsigned)usage.peak_active_slots,
             (unsigned)usage.started_requests,
             (unsigned)usage.completed_requests,
             (unsigned)usage.cancelled_requests,
             (unsigned)usage.timed_out_requests,
             (unsigned)usage.queue_full_requests,
             (unsigned)usage.response_bytes,
             (unsigned)usage.peak_response_bytes,
             (unsigned)usage.slot_storage_bytes);
}

static void host_log_activation_memory_usage(void) {
    pxa_host_activation_memory_snapshot_t usage;
    pxa_host_activation_arena_snapshot(&g_host.activation.activation_memory, &usage);
    if (usage.block_count == 0) return;
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Activation memory: workspaces=%u blocks=%u used=%u reserved=%u "
             "peak_used=%u peak_reserved=%u",
             (unsigned)usage.workspace_count, (unsigned)usage.block_count,
             (unsigned)usage.used_bytes, (unsigned)usage.reserved_bytes,
             (unsigned)usage.peak_used_bytes,
             (unsigned)usage.peak_reserved_bytes);
}

static pxa_status_t prepare_start(void *context, pxa_component_t component,
                                  uint64_t instance_id, uint8_t kind);
static pxa_status_t read_artifact(void *host_context, pxa_bytes_t path,
                                  uint8_t *output, size_t capacity,
                                  size_t *size);

static pxa_status_t reset_runtime(uint16_t max_components) {
    pxa_runtime_limits_t limits;
    pxa_service_ops_t operations;
    pxa_status_t status;
    size_t workspace_size;

    if (max_components == 0) return PXA_STATUS_INVALID_ARGUMENT;
    host_runtime_limits_init(&limits, max_components);
    workspace_size = pxa_runtime_workspace_size(&limits);
    if (workspace_size == 0) return PXA_STATUS_INVALID_ARGUMENT;
    if (g_host.activation.runtime != NULL) pxa_runtime_deinit(g_host.activation.runtime);
    g_host.activation.runtime = NULL;
    if (g_host.engine != NULL)
        pxa_wamr_engine_set_runtime(g_host.engine, NULL);
    free(g_host.runtime_workspace);
    g_host.runtime_workspace = esp_alloc(workspace_size);
    if (g_host.runtime_workspace == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    status = pxa_runtime_init(g_host.runtime_workspace, workspace_size, &limits,
                              &g_host.activation.runtime);
    if (status != PXA_STATUS_OK) goto failed;

    memset(&operations, 0, sizeof(operations));
    operations.struct_size = sizeof(operations);
    operations.service_id = PXA_CLOCK_SERVICE_ID;
    operations.major = PXA_CORE_SERVICE_MAJOR;
    operations.minor = PXA_CORE_SERVICE_MINOR;
    operations.control = host_clock_control;
    status = pxa_service_register(g_host.activation.runtime, &operations);
    if (status != PXA_STATUS_OK) goto failed;

    memset(&operations, 0, sizeof(operations));
    operations.struct_size = sizeof(operations);
    operations.service_id = PXA_HOST_SYSTEM_SERVICE_ID;
    operations.major = PXA_HOST_SYSTEM_SERVICE_MAJOR;
    operations.minor = PXA_HOST_SYSTEM_SERVICE_MINOR;
    operations.control = host_system_control;
    status = pxa_service_register(g_host.activation.runtime, &operations);
    if (status != PXA_STATUS_OK) goto failed;

    memset(&operations, 0, sizeof(operations));
    operations.struct_size = sizeof(operations);
    operations.service_id = PXA_STORE_INSTALL_SERVICE_ID;
    operations.major = PXA_STORE_INSTALL_SERVICE_MAJOR;
    operations.minor = PXA_STORE_INSTALL_SERVICE_MINOR;
    operations.control = host_store_install_control;
    status = pxa_service_register(g_host.activation.runtime, &operations);
    if (status != PXA_STATUS_OK) goto failed;

    if (g_host.engine != NULL) {
        pxa_wamr_engine_set_runtime(g_host.engine, g_host.activation.runtime);
    }
    return PXA_STATUS_OK;

failed:
    if (g_host.activation.runtime != NULL)
        pxa_runtime_deinit(g_host.activation.runtime);
    g_host.activation.runtime = NULL;
    free(g_host.runtime_workspace);
    g_host.runtime_workspace = NULL;
    return status;
}

static pxa_status_t initialize_engine(uint16_t max_components) {
    pxa_wamr_engine_config_t config;
    size_t workspace_size;
    pxa_status_t status;
    if (max_components == 0 || g_host.activation.runtime == NULL) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    if (g_host.engine != NULL) {
        pxa_wamr_engine_set_runtime(g_host.engine, NULL);
        pxa_wamr_engine_deinit(g_host.engine);
        g_host.engine = NULL;
    }
    free(g_host.engine_workspace);
    g_host.engine_workspace = NULL;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.read_artifact = read_artifact;
    config.now_us = host_now_us;
    config.prepare_start = prepare_start;
    config.call_timeout_us = (uint64_t)CONFIG_PXA_CALL_TIMEOUT_MS * 1000u;
    config.guest_stack_size = CONFIG_PXA_GUEST_STACK_SIZE;
    config.host_managed_heap_size = CONFIG_PXA_HOST_MANAGED_HEAP_SIZE;
    config.max_components = max_components;
#ifdef CONFIG_PXA_WASI_LIBC
    config.wasi_enabled = 1;
#endif
    config.max_module_bytes = 0;
    config.synchronization_context = &g_wamr_watchdog_lock;
    config.enter_critical = enter_wamr_watchdog_critical;
    config.leave_critical = leave_wamr_watchdog_critical;
    config.allocate_artifact = esp_artifact_alloc;
    config.release_artifact = esp_artifact_free;
    config.allocate_runtime = esp_wamr_alloc;
    config.reallocate_runtime = esp_wamr_realloc;
    config.release_runtime = esp_wamr_free;
    workspace_size = pxa_wamr_engine_workspace_size(&config);
    if (workspace_size == 0) return PXA_STATUS_INVALID_ARGUMENT;
    g_host.engine_workspace = esp_alloc(workspace_size);
    if (g_host.engine_workspace == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    status = pxa_wamr_engine_init(g_host.engine_workspace, workspace_size,
                                  &config, &g_host.engine,
                                  &g_host.engine_ops);
    if (status != PXA_STATUS_OK) {
        free(g_host.engine_workspace);
        g_host.engine_workspace = NULL;
        return status;
    }
    pxa_wamr_engine_set_runtime(g_host.engine, g_host.activation.runtime);
    return PXA_STATUS_OK;
}

/* --- sensor/audio providers --------------------------------------------- */

/* --- services ----------------------------------------------------------- */

static void destroy_services(void) {
    pxa_esp_services_destroy(&g_host.activation.services);
    g_host.activation.ui_component = PXA_COMPONENT_INVALID;
}

/* --- engine artifacts ---------------------------------------------------- */

static pxa_status_t read_artifact(void *host_context, pxa_bytes_t path,
                                  uint8_t *output, size_t capacity,
                                  size_t *size) {
    FILE *file;
    long file_size;
    (void)host_context;
    if (path.data == NULL || size == NULL ||
        (output == NULL && capacity != 0)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    file = fopen((const char *)path.data, "rb");
    if (file == NULL) return PXA_STATUS_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return PXA_STATUS_INTERNAL;
    }
    file_size = ftell(file);
    if (file_size <= 0) {
        fclose(file);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (output == NULL) {
        fclose(file);
        *size = (size_t)file_size;
        return PXA_STATUS_OK;
    }
    if ((size_t)file_size > capacity) {
        fclose(file);
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return PXA_STATUS_INTERNAL;
    }
    if (fread(output, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        return PXA_STATUS_INTERNAL;
    }
    fclose(file);
    *size = (size_t)file_size;
    return PXA_STATUS_OK;
}

/* --- app start/stop ------------------------------------------------------ */

static void stop_active(pxa_stop_reason_t reason) {
    pxa_esp_host_public_state_t public_state;
    char stopped_identity[PXA_ESP_HOST_MAX_APP_ID];
    public_state_snapshot(&public_state);
    snprintf(stopped_identity, sizeof(stopped_identity), "%s",
             g_host.activation.active_identity);
    if (g_store_install != NULL && g_store_install->prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_permission_prompt(g_store_install->prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        g_store_install->prompt_id = 0;
        g_store_install->approved = 0;
        xTaskNotifyGive(g_store_install->worker);
    }
    if (g_store_install != NULL && g_store_install->separate &&
        g_store_install->ready) {
        g_store_install->ready = 0;
        g_store_install->approved = 0;
        xTaskNotifyGive(g_store_install->worker);
    }
    if (g_store_uninstall.prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_permission_prompt(g_store_uninstall.prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        if (g_host.activation.runtime != NULL)
            (void)pxa_request_cancel(g_host.activation.runtime,
                                     g_store_uninstall.component,
                                     g_store_uninstall.request_id);
        memset(&g_store_uninstall, 0, sizeof(g_store_uninstall));
    }
    if (g_store_result.prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_store_result(g_store_result.prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        memset(&g_store_result, 0, sizeof(g_store_result));
    }
    unpublish_active_state();
    dismiss_runtime_permission_prompt();
    dismiss_unresponsive_prompt();
    host_log_ui_usage();
    host_log_audio_usage();
    host_log_net_usage();
    if (g_host.activation.coordinator != NULL) {
        pxa_activation_deactivate_all(g_host.activation.coordinator, reason);
    }
    g_host.activation.coordinator = NULL;
    g_host.activation.coordinator_workspace = NULL;
    g_host.activation.plan_workspace = NULL;
    g_host.activation.services.window_workspace = NULL;
    g_host.activation.services.window = NULL;
    pxa_lvgl_ui_reset(g_host.ui_adapter);
    pxa_esp_ui_assets_clear();
    g_host.activation.active_manifest = NULL;
    g_host.activation.active_package_root[0] = '\0';
    g_host.activation.manifest_workspace_keep = NULL;
    g_host.activation.encoded_keep = NULL;
    g_host.activation.active_component = PXA_COMPONENT_INVALID;
    g_host.activation.active_instance_id = 0;
    clear_pointer_mailbox();
    g_host.activation.active_component_count = 0;
    g_host.activation.active_component_capacity = 0;
    g_host.activation.active_components = NULL;
    g_host.activation.active_jobs = NULL;
    g_host.activation.job_stop_at_ms = NULL;
    g_host.activation.job_force_stop_at_ms = NULL;
    g_host.activation.active_work = NULL;
    g_host.activation.work_result = NULL;
    g_host.activation.activating_job_slot = -1;
    g_host.activation.active_identity[0] = '\0';
    g_host.activation.active_name[0] = '\0';
    if (public_state.main_active)
        notify_runtime_event(PXA_HOST_RUNTIME_STOPPED, stopped_identity);
    portENTER_CRITICAL(&g_clock_slots_lock);
    pxa_host_clock_slots_cancel_all(&g_host.clock_slots);
    portEXIT_CRITICAL(&g_clock_slots_lock);
    host_log_runtime_usage();
    host_log_wamr_usage();
    host_log_activation_memory_usage();
    if (g_host.engine != NULL) {
        pxa_wamr_engine_set_runtime(g_host.engine, NULL);
        pxa_wamr_engine_deinit(g_host.engine);
        g_host.engine = NULL;
    }
    if (g_host.activation.runtime != NULL) {
        pxa_runtime_deinit(g_host.activation.runtime);
        g_host.activation.runtime = NULL;
    }
    destroy_services();
    pxa_esp_audio_reset_sessions();
    pxa_esp_net_reset_requests();
    free(g_host.engine_workspace);
    g_host.engine_workspace = NULL;
    free(g_host.runtime_workspace);
    g_host.runtime_workspace = NULL;
    activation_memory_release_all();
    activation_state_reset();
}

static int start_verified(const char *identity) {
    pxa_package_manifest_t *manifest = NULL;
    pxa_activation_plan_t *plan = NULL;
    pxa_package_host_profile_t host_profile;
    pxa_package_activation_profile_t capabilities;
    pxa_esp_services_config_t services_config;
    pxa_esp_services_result_t services_result;
    pxa_package_service_capability_t service_capabilities
        [PXA_ESP_HOST_MAX_SERVICES];
    char root[PXA_ESP_HOST_MAX_PATH];
    uint8_t *encoded = NULL;
    void *manifest_workspace = NULL;
    size_t manifest_workspace_size;
    size_t encoded_size;
    size_t plan_size;
    size_t workspace_size;
    size_t index;
    uint64_t main_instance;
    pxa_status_t status = PXA_STATUS_INTERNAL;
    const char *failed_stage;
    int toast_posted = 0;
    if (!pxa_esp_package_store_is_enabled(identity)) {
        ESP_LOGW(PXA_ESP_HOST_TAG, "Launch denied for disabled Package %s",
                 identity);
        return 0;
    }
    encoded_size =
        pxa_esp_package_store_installed_manifest_size(identity);
    if (encoded_size == 0) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Package store did not provide manifest size for %s",
                 identity);
        return 0;
    }
    workspace_size =
        pxa_esp_package_store_installed_manifest_workspace_size(identity);
    manifest_workspace_size = workspace_size;
    if (workspace_size == 0) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Package store did not provide manifest workspace for %s",
                 identity);
        return 0;
    }
    encoded = esp_alloc(encoded_size);
    if (encoded == NULL) return 0;
    manifest_workspace = esp_alloc(workspace_size);
    if (manifest_workspace == NULL) {
        free(encoded);
        return 0;
    }
    if (!pxa_esp_package_store_load_installed(
            identity, root, sizeof(root), manifest_workspace,
            workspace_size, encoded, encoded_size,
            &manifest)) {
        free(manifest_workspace);
        free(encoded);
        pxa_esp_ui_shell_post_toast("应用验证失败", 1800);
        return 0;
    }
    stop_active(PXA_STOP_REPLACED);
    host_log_heap_usage("launch-begin");
    if (!activation_memory_begin(manifest_workspace, manifest_workspace_size,
                                 encoded, encoded_size)) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Activation memory ownership transfer failed for %s",
                 identity);
        free(manifest_workspace);
        free(encoded);
        return 0;
    }
    /* Manifest views reference encoded + manifest_workspace; keep both
     * alive until activation completes. */
    g_host.activation.manifest_workspace_keep = manifest_workspace;
    g_host.activation.encoded_keep = encoded;
    g_host.activation.active_manifest = manifest;
    g_host.activation.active_components = activation_alloc(
        (size_t)manifest->component_count *
        sizeof(g_host.activation.active_components[0]));
    if (g_host.activation.active_components == NULL) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Active component table allocation failed for %s", identity);
        stop_active(PXA_STOP_FAULT);
        return 0;
    }
    g_host.activation.active_component_capacity = manifest->component_count;
    snprintf(g_host.activation.active_package_root, sizeof(g_host.activation.active_package_root),
             "%s", root);
    if (!pxa_esp_ui_assets_begin(manifest,
                                 g_host.activation.active_package_root)) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "UI asset cache initialization failed for %s", identity);
        stop_active(PXA_STOP_FAULT);
        return 0;
    }
    if (!pxa_esp_audio_bind_package(
            manifest, g_host.activation.active_package_root)) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Audio asset binding failed for %s", identity);
        stop_active(PXA_STOP_FAULT);
        return 0;
    }

#define PXA_ESP_START_FAIL(stage_name, failure_status)                         \
    do {                                                                         \
        failed_stage = (stage_name);                                            \
        status = (failure_status);                                              \
        goto failed;                                                            \
    } while (0)

    status = reset_runtime(manifest->component_count);
    if (status != PXA_STATUS_OK) {
        PXA_ESP_START_FAIL("reset-runtime", status);
    }
    status = initialize_engine(manifest->component_count);
    if (status != PXA_STATUS_OK) {
        PXA_ESP_START_FAIL("initialize-engine", status);
    }
    memset(&services_config, 0, sizeof(services_config));
    services_config.runtime = g_host.activation.runtime;
    services_config.manifest = manifest;
    services_config.identity = identity;
    services_config.allocator_context =
        &g_host.activation.activation_memory;
    services_config.allocate = activation_workspace_alloc;
    services_config.permission_prompt = request_runtime_permission_prompt;
    services_config.net_notify = on_net_completion_ready;
    services_config.work_context = NULL;
    services_config.complete_work = complete_work;
    services_config.cancel_work = cancel_work;
    services_config.work_epoch = g_host.work_epoch;
    services_config.color_scheme = g_host.color_scheme;
    {
        /* The UI service otherwise starts from a legacy 320x240 default, so
         * Guests would lay out for the wrong panel. Seed it with the real
         * display metrics and safe insets before any component binds. */
        lv_display_t *display;
        lv_lock();
        display = lv_display_get_default();
        if (display != NULL) {
            services_config.primary_width =
                (uint32_t)lv_display_get_horizontal_resolution(display);
            services_config.primary_height =
                (uint32_t)lv_display_get_vertical_resolution(display);
        }
        lv_unlock();
        portENTER_CRITICAL(&g_process_state_lock);
        if (g_host.window_insets_valid) {
            services_config.safe_insets[0] = g_host.safe_insets.top;
            services_config.safe_insets[1] = g_host.safe_insets.right;
            services_config.safe_insets[2] = g_host.safe_insets.bottom;
            services_config.safe_insets[3] = g_host.safe_insets.left;
        }
        services_config.display_shape = g_host.display_shape;
        memcpy(services_config.corner_radii, g_host.corner_radii,
               sizeof(services_config.corner_radii));
        portEXIT_CRITICAL(&g_process_state_lock);
    }
    status = pxa_esp_services_initialize(
        &g_host.activation.services, &services_config, &services_result);
    if (status != PXA_STATUS_OK) {
        if (services_result.issue ==
            PXA_ESP_SERVICES_ISSUE_INVALID_PERMISSIONS) {
            pxa_esp_ui_shell_post_toast("应用权限声明无效", 1800);
            toast_posted = 1;
        } else if (services_result.issue ==
                   PXA_ESP_SERVICES_ISSUE_REQUIRED_PERMISSION_DENIED) {
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Launch denied by required permission policy for %s",
                     identity);
            pxa_esp_ui_shell_post_toast("应用所需权限未授权", 1800);
            toast_posted = 1;
        } else if (services_result.issue ==
                   PXA_ESP_SERVICES_ISSUE_INVALID_JOB) {
            pxa_esp_ui_shell_post_toast("应用任务声明无效", 1800);
            toast_posted = 1;
        }
        PXA_ESP_START_FAIL(
            services_result.stage == NULL ? "initialize-services"
                                          : services_result.stage,
            status);
    }
    if (g_host.activation.services.ui != NULL) {
        pxa_ui_theme_snapshot_t theme = {0};
        static const uint16_t sizes[PXA_UI_THEME_FONT_COUNT] = {
            12u, 14u, 16u, 20u, 24u, 28u
        };
        theme.generation = g_host.ui_theme_generation;
        theme.color_scheme = g_host.color_scheme;
        memcpy(theme.rgba, g_host.ui_theme.rgba, sizeof(theme.rgba));
        memcpy(theme.typography_px, sizes, sizeof(sizes));
        (void)pxa_ui_update_theme(g_host.activation.services.ui, &theme);
    }
    if (!allocate_job_slots(
            g_host.activation.services.job_component_count)) {
        PXA_ESP_START_FAIL("allocate-job-slots", PXA_STATUS_RESOURCE_LIMIT);
    }
    /* Activation plan + coordinator. */
    memset(&host_profile, 0, sizeof(host_profile));
    host_profile.target =
        (pxa_bytes_t){(const uint8_t *)PXA_ESP_HOST_TARGET,
                      sizeof(PXA_ESP_HOST_TARGET) - 1u};
    host_profile.engine = (pxa_bytes_t){(const uint8_t *)"wamr", 4};
    host_profile.engine_abi =
        (pxa_bytes_t){(const uint8_t *)PXSYS_WAMR_ENGINE_ABI,
                      sizeof(PXSYS_WAMR_ENGINE_ABI) - 1u};
    host_profile.memory_model = PXA_MEMORY_WASM32;
    memset(&capabilities, 0, sizeof(capabilities));
    {
#define HOST_SERVICE_VERSION(name) \
    { PXA_##name##_SERVICE_ID, \
      { PXA_##name##_SERVICE_MAJOR, PXA_##name##_SERVICE_MINOR }, 0 }
        static const pxa_package_service_capability_t registered_versions[] = {
            HOST_SERVICE_VERSION(CORE),
            HOST_SERVICE_VERSION(WINDOW),
            HOST_SERVICE_VERSION(UI),
            { PXA_CLOCK_SERVICE_ID,
              { PXA_CORE_SERVICE_MAJOR, PXA_CORE_SERVICE_MINOR }, 0 },
            HOST_SERVICE_VERSION(FS),
            HOST_SERVICE_VERSION(STORAGE),
            HOST_SERVICE_VERSION(IPC),
            HOST_SERVICE_VERSION(SENSOR),
            HOST_SERVICE_VERSION(NET),
            HOST_SERVICE_VERSION(AUDIO),
            HOST_SERVICE_VERSION(PERMISSION),
            HOST_SERVICE_VERSION(WORK),
            HOST_SERVICE_VERSION(SURFACE),
            HOST_SERVICE_VERSION(GAME_RENDER),
            HOST_SERVICE_VERSION(LOG),
#ifdef CONFIG_PXA_WASI_LIBC
            HOST_SERVICE_VERSION(WASI),
#endif
            HOST_SERVICE_VERSION(DEVICE),
            HOST_SERVICE_VERSION(HOST_SYSTEM),
            HOST_SERVICE_VERSION(STORE_INSTALL),
        };
#undef HOST_SERVICE_VERSION
        memcpy(service_capabilities, registered_versions,
               sizeof(registered_versions));
        capabilities.core_version.major = PXA_CORE_VERSION_MAJOR;
        capabilities.core_version.minor = PXA_CORE_VERSION_MINOR;
        capabilities.services = service_capabilities;
        capabilities.service_count =
            (uint16_t)(sizeof(registered_versions) /
                       sizeof(registered_versions[0]));
        for (index = 0; index < capabilities.service_count; ++index) {
#ifdef CONFIG_PXA_WASI_LIBC
            if (service_capabilities[index].service == PXA_WASI_SERVICE_ID) {
                service_capabilities[index].features =
                    PXA_WASI_FEATURE_CLOCKS | PXA_WASI_FEATURE_RANDOM;
            }
#endif
            if (service_capabilities[index].service == PXA_UI_SERVICE_ID) {
                service_capabilities[index].features =
                    PXA_UI_FEATURE_CANVAS | PXA_UI_FEATURE_VIRTUAL_LIST |
        PXA_UI_FEATURE_GRID |
                    PXA_UI_FEATURE_RGB565_BITMAP |
                    PXA_UI_FEATURE_CONTROLLER_INPUT |
                    PXA_UI_FEATURE_CANVAS_STREAM_IO;
            }
        }
    }
    plan_size =
        pxa_activation_plan_workspace_size(manifest->component_count);
    g_host.activation.plan_workspace = activation_alloc(plan_size);
    if (g_host.activation.plan_workspace == NULL) {
        PXA_ESP_START_FAIL("allocate-activation-plan", PXA_STATUS_RESOURCE_LIMIT);
    }
    status = pxa_activation_plan_prepare(
        g_host.activation.plan_workspace, plan_size, manifest, &capabilities,
        &host_profile,
        (pxa_bytes_t){
            (const uint8_t *)g_host.activation.active_package_root,
            strlen(g_host.activation.active_package_root)},
        &plan);
    if (status != PXA_STATUS_OK) {
        char reason[128];
        log_activation_compatibility(manifest, &capabilities, &host_profile,
                                     reason, sizeof(reason));
        pxa_esp_ui_shell_post_toast(
            reason[0] != '\0' ? reason : "应用与设备不兼容", 5000);
        toast_posted = 1;
        PXA_ESP_START_FAIL("prepare-activation-plan", status);
    }
    {
        size_t coordinator_size =
            pxa_activation_coordinator_workspace_size(plan);
        g_host.activation.coordinator_workspace = activation_alloc(coordinator_size);
        if (g_host.activation.coordinator_workspace == NULL) {
            PXA_ESP_START_FAIL("allocate-activation-coordinator",
                               PXA_STATUS_RESOURCE_LIMIT);
        }
        status = pxa_activation_coordinator_init(
            g_host.activation.coordinator_workspace, coordinator_size, g_host.activation.runtime,
            plan, &g_host.engine_ops, &g_host.activation.coordinator);
        if (status != PXA_STATUS_OK) {
            PXA_ESP_START_FAIL("initialize-activation-coordinator", status);
        }
    }
    snprintf(g_host.activation.active_identity, sizeof(g_host.activation.active_identity), "%s",
             identity);
    copy_permission_prompt_name(g_host.activation.active_name, sizeof(g_host.activation.active_name),
                                manifest->name, identity);
    status = pxa_ipc_broker_set_allocator(
        g_host.activation.services.ipc, NULL, ipc_alloc, ipc_release);
    if (status != PXA_STATUS_OK) {
        PXA_ESP_START_FAIL("configure-ipc-allocator", status);
    }
    status = pxa_ipc_broker_set_endpoint_resolver(
        g_host.activation.services.ipc, NULL, resolve_ipc_endpoint);
    if (status != PXA_STATUS_OK) {
        PXA_ESP_START_FAIL("configure-ipc-endpoint-resolver", status);
    }
    /* Reserve endpoint names now; providers are activated by their first
     * caller so unused services consume no WAMR instance memory. */
    for (index = 0; index < manifest->ipc_endpoint_count; ++index) {
        pxa_package_ipc_endpoint_t *endpoint = &manifest->ipc_endpoints[index];
        status = pxa_ipc_endpoint_declare(g_host.activation.services.ipc,
                                          endpoint->name);
        if (status != PXA_STATUS_OK) {
            pxa_esp_ui_shell_post_toast("应用 IPC 初始化失败", 1800);
            toast_posted = 1;
            PXA_ESP_START_FAIL("declare-ipc-endpoint", status);
        }
    }
    main_instance = allocate_instance_id();
    status = activate_component_profiled(
        (pxa_bytes_t){(const uint8_t *)"main", 4}, main_instance,
        &g_host.activation.active_component);
    if (status != PXA_STATUS_OK) {
        pxa_esp_ui_shell_post_toast("应用启动失败", 1800);
        toast_posted = 1;
        PXA_ESP_START_FAIL("activate-main-component", status);
    }
    if (!track_active_component(g_host.activation.active_component)) {
        PXA_ESP_START_FAIL("track-main-component", PXA_STATUS_RESOURCE_LIMIT);
    }
    g_host.activation.active_instance_id = main_instance;
    update_window_snapshot(g_host.activation.active_component);
    (void)pxa_ipc_flush(g_host.activation.services.ipc);
    if (!drain_active_events()) {
        PXA_ESP_START_FAIL("drain-startup-events", PXA_STATUS_INTERNAL);
    }
    pxa_esp_ui_shell_dismiss_app_launch();
    publish_active_state();
    refresh_volume_key_capture_state();
    host_log_heap_usage("launch-ready");
    ESP_LOGI(PXA_ESP_HOST_TAG, "Started verified Package %s", identity);
    return 1;

failed:
    ESP_LOGE(PXA_ESP_HOST_TAG, "Package launch failed at %s: id=%s status=%d",
             failed_stage, identity, (int)status);
    host_log_heap_usage("launch-failed");
    stop_active(PXA_STOP_FAULT);
    host_log_heap_usage("launch-cleanup");
    pxa_esp_ui_shell_dismiss_app_launch();
    if (!toast_posted) {
        pxa_esp_ui_shell_post_toast("应用启动失败", 1800);
    }
    return 0;
}

#undef PXA_ESP_START_FAIL

/* --- timer callback (5 ms: maintenance + clock slots) -------------------- */

static void on_clock_timer(void *arg) {
    pxa_host_clock_tick_t ticks[PXA_HOST_CLOCK_SLOT_COUNT];
    pxa_esp_host_command_t command;
    size_t count;
    size_t index;
    uint64_t now_us;
    (void)arg;
    now_us = host_now_us(NULL);
    /* Keep the newest move buffered and wake the guest at display cadence. */
    if (pointer_mailbox_has_move()) wake_runtime_thread();
    if (now_us >= g_host.next_maintenance_us) {
        g_host.next_maintenance_us =
            now_us + PXA_ESP_HOST_MAINTENANCE_PERIOD_US;
        (void)post_maintenance(PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE);
        (void)post_maintenance(PXA_ESP_HOST_CMD_NET_MAINTENANCE);
        (void)post_maintenance(PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE);
    }
    if (now_us >= g_host.next_lifecycle_us) {
        g_host.next_lifecycle_us =
            now_us + PXA_ESP_HOST_LIFECYCLE_PERIOD_US;
        (void)post_maintenance(PXA_ESP_HOST_CMD_LEASE_MAINTENANCE);
        mark_scheduler_maintenance_due();
    }
    portENTER_CRITICAL(&g_clock_slots_lock);
    count = pxa_host_clock_slots_take_due(
        &g_host.clock_slots, now_us, ticks, PXA_HOST_CLOCK_SLOT_COUNT);
    portEXIT_CRITICAL(&g_clock_slots_lock);
    for (index = 0; index < count; ++index) {
        memset(&command, 0, sizeof(command));
        command.type = (uint8_t)PXA_ESP_HOST_CMD_TICK;
        command.payload.tick.slot = ticks[index].slot;
        command.payload.tick.generation = ticks[index].generation;
        if (!post_command(&command)) {
            portENTER_CRITICAL(&g_clock_slots_lock);
            pxa_host_clock_slots_post_failed(
                &g_host.clock_slots, ticks[index].slot,
                ticks[index].generation);
            portEXIT_CRITICAL(&g_clock_slots_lock);
        }
    }
}

/* --- watchdog: ask before terminating overdue guest calls ---------------- */

static void on_watchdog_timer(void *arg) {
    pxa_esp_ui_unresponsive_prompt_t prompt;
    pxa_esp_host_public_state_t public_state;
    uint32_t prompt_id;
    int prompt_pending;
    (void)arg;
    if (g_host.engine == NULL) return;
    /* A package can only have one actionable operator decision. Further
     * deadline expirations are folded into the existing prompt. */
    portENTER_CRITICAL(&g_wamr_watchdog_lock);
    prompt_pending = g_host.unresponsive_prompt_id != 0;
    portEXIT_CRITICAL(&g_wamr_watchdog_lock);
    if (prompt_pending) return;
    if (!pxa_wamr_engine_take_expired_deadline(g_host.engine,
                                               host_now_us(NULL))) {
        return;
    }
    portENTER_CRITICAL(&g_wamr_watchdog_lock);
    if (g_host.unresponsive_prompt_id != 0) {
        portEXIT_CRITICAL(&g_wamr_watchdog_lock);
        return;
    }
    prompt_id = ++g_host.next_unresponsive_prompt_id;
    if (prompt_id == 0) prompt_id = ++g_host.next_unresponsive_prompt_id;
    g_host.unresponsive_prompt_id = prompt_id;
    portEXIT_CRITICAL(&g_wamr_watchdog_lock);
    public_state_snapshot(&public_state);
    if (!public_state.package_active) {
        portENTER_CRITICAL(&g_wamr_watchdog_lock);
        if (g_host.unresponsive_prompt_id == prompt_id)
            g_host.unresponsive_prompt_id = 0;
        portEXIT_CRITICAL(&g_wamr_watchdog_lock);
        return;
    }
    memset(&prompt, 0, sizeof(prompt));
    prompt.prompt_id = prompt_id;
    copy_utf8_c_string(prompt.app_name, sizeof(prompt.app_name),
                       public_state.name[0] == '\0' ? public_state.identity
                                                    : public_state.name);
    pxa_esp_surface_runtime_modal_enter();
    if (!pxa_esp_ui_shell_post_unresponsive_prompt(&prompt)) {
        portENTER_CRITICAL(&g_wamr_watchdog_lock);
        if (g_host.unresponsive_prompt_id == prompt_id)
            g_host.unresponsive_prompt_id = 0;
        portEXIT_CRITICAL(&g_wamr_watchdog_lock);
        pxa_esp_surface_runtime_modal_leave();
        (void)pxa_wamr_engine_terminate_active_call(g_host.engine);
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Guest callback exceeded %u ms; prompt unavailable, terminating it",
                 (unsigned)CONFIG_PXA_CALL_TIMEOUT_MS);
        return;
    }
    ESP_LOGW(PXA_ESP_HOST_TAG,
             "Guest callback exceeded %u ms; awaiting user decision",
             (unsigned)CONFIG_PXA_CALL_TIMEOUT_MS);
}

/* --- command loop -------------------------------------------------------- */

static void post_pointer(const pxa_esp_host_command_t *command) {
    const pxa_host_pointer_event_t *event = &command->payload.pointer;
    if (g_host.activation.services.ui == NULL || g_host.activation.ui_component == PXA_COMPONENT_INVALID ||
        event->instance_id == 0 ||
        event->instance_id != g_host.activation.active_instance_id)
        return;
    {
        uint8_t value[12] = {0};
        value[0] = event->id;
        value[1] = event->phase;
        pxa_write_u32(value + 4, (uint32_t)event->x);
        pxa_write_u32(value + 8, (uint32_t)event->y);
        pxa_esp_surface_note_input_sample(event->timestamp_us);
        if (event->phase != PXA_HOST_POINTER_MOVE_PHASE) {
            ESP_LOGI(PXA_ESP_HOST_TAG,
                     "pointer delivered id=%u phase=%u x=%d y=%d",
                     (unsigned)event->id, (unsigned)event->phase,
                     (int)event->x, (int)event->y);
        }
        (void)pxa_ui_queue_event(
            g_host.activation.services.ui, g_host.activation.ui_component,
            event->surface, event->node, PXA_UI_EVENT_POINTER,
            event->phase == PXA_HOST_POINTER_MOVE_PHASE
                ? 0 : PXA_UI_EVENT_FLAG_RELIABLE,
            event->timestamp_us, value, sizeof(value));
    }
    drain_active_events();
    pxa_esp_surface_note_input_delivered(event->timestamp_us,
                                         host_now_us(NULL));
}

static void post_ui_event(const pxa_esp_host_command_t *command) {
    const pxa_host_ui_event_t *event = &command->payload.ui_event;
    const int has_value = event->kind == PXA_UI_EVENT_VALUE_CHANGED ||
                          event->kind == PXA_UI_EVENT_SCROLL ||
                          event->kind == PXA_UI_EVENT_KEY;
    const int has_text = event->kind == PXA_UI_EVENT_TEXT &&
                         event->text_size != 0;
    if (g_host.activation.services.ui == NULL || g_host.activation.ui_component == PXA_COMPONENT_INVALID)
        return;
    if (event->instance_id == 0 ||
        event->instance_id != g_host.activation.active_instance_id) {
        return;
    }
    (void)pxa_ui_queue_event(
        g_host.activation.services.ui, g_host.activation.ui_component,
        event->surface, event->node, event->kind, event->flags,
        event->timestamp_us,
        has_text ? (const void *)event->text
                 : has_value ? &event->value : NULL,
        has_text ? event->text_size
                 : has_value ? sizeof(event->value) : 0);
    drain_active_events();
}

static void post_controller(const pxa_esp_host_command_t *command) {
    const pxa_host_controller_event_t *event =
        &command->payload.controller;
    uint8_t value[8] = {0};
    if (g_host.activation.services.ui == NULL ||
        g_host.activation.ui_component == PXA_COMPONENT_INVALID ||
        event->instance_id == 0 ||
        event->instance_id != g_host.activation.active_instance_id)
        return;
    value[0] = event->controller;
    value[1] = event->connected;
    pxa_write_u32(value + 4, event->buttons);
    (void)pxa_ui_queue_event(
        g_host.activation.services.ui, g_host.activation.ui_component,
        PXA_UI_PRIMARY_SURFACE, 1u, PXA_UI_EVENT_CONTROLLER_STATE,
        PXA_UI_EVENT_FLAG_COALESCIBLE, event->timestamp_us, value,
        sizeof(value));
    drain_active_events();
}

static void apply_color_scheme(pxa_ui_color_scheme_t color_scheme) {
    pxa_ui_environment_t environment;
    if (g_host.color_scheme == color_scheme) return;
    g_host.color_scheme = color_scheme;
    if (g_host.activation.services.ui != NULL) {
        pxa_ui_theme_snapshot_t theme;
        if (pxa_ui_get_theme(g_host.activation.services.ui, &theme) == PXA_STATUS_OK) {
            theme.color_scheme = color_scheme;
            theme.generation = ++g_host.ui_theme_generation;
            if (theme.generation == 0) theme.generation = ++g_host.ui_theme_generation;
            (void)pxa_ui_update_theme(g_host.activation.services.ui, &theme);
            drain_active_events();
        }
    }
    if (g_host.activation.services.ui == NULL ||
        g_host.activation.ui_component == PXA_COMPONENT_INVALID)
        return;
    if (pxa_ui_get_environment(g_host.activation.services.ui,
                               g_host.activation.ui_component,
                               PXA_UI_PRIMARY_SURFACE, &environment) !=
        PXA_STATUS_OK)
        return;
    if (environment.color_scheme == color_scheme)
        return;
    environment.color_scheme = color_scheme;
    if (pxa_ui_update_environment(g_host.activation.services.ui,
                                  g_host.activation.ui_component,
                                  &environment) == PXA_STATUS_OK)
        drain_active_events();
}

static void apply_ui_palette(const uint32_t rgba[PXA_UI_THEME_ROLE_COUNT]) {
    if (memcmp(g_host.ui_theme.rgba, rgba,
               sizeof(g_host.pending_ui_palette)) == 0) return;
    memcpy(g_host.ui_theme.rgba, rgba, sizeof(g_host.pending_ui_palette));
    if (g_host.ui_adapter != NULL)
        (void)pxa_lvgl_ui_set_theme(g_host.ui_adapter, &g_host.ui_theme);
    if (g_host.activation.services.ui != NULL) {
        pxa_ui_theme_snapshot_t theme;
        if (pxa_ui_get_theme(g_host.activation.services.ui, &theme) == PXA_STATUS_OK) {
            theme.color_scheme = g_host.color_scheme;
            theme.generation = ++g_host.ui_theme_generation;
            if (theme.generation == 0) theme.generation = ++g_host.ui_theme_generation;
            memcpy(theme.rgba, rgba, sizeof(theme.rgba));
            (void)pxa_ui_update_theme(g_host.activation.services.ui, &theme);
            drain_active_events();
        }
    } else {
        ++g_host.ui_theme_generation;
        if (g_host.ui_theme_generation == 0) ++g_host.ui_theme_generation;
    }
}

static void apply_window_insets(const pxa_window_insets_t *safe_insets,
                                const pxa_window_insets_t *system_bar_insets) {
    pxa_ui_environment_t environment;
    uint32_t display_shape;
    uint32_t corner_radii[4];
    lv_display_t *display;
    lv_lock();
    display = lv_display_get_default();
    lv_unlock();
    portENTER_CRITICAL(&g_process_state_lock);
    if (safe_insets != NULL) g_host.safe_insets = *safe_insets;
    if (system_bar_insets != NULL) g_host.system_bar_insets = *system_bar_insets;
    g_host.window_insets_valid = 1;
    display_shape = g_host.display_shape;
    memcpy(corner_radii, g_host.corner_radii, sizeof(corner_radii));
    portEXIT_CRITICAL(&g_process_state_lock);
    if (g_host.activation.services.ui == NULL ||
        g_host.activation.ui_component == PXA_COMPONENT_INVALID ||
        pxa_ui_get_environment(g_host.activation.services.ui,
                               g_host.activation.ui_component,
                               PXA_UI_PRIMARY_SURFACE, &environment) !=
            PXA_STATUS_OK)
        return;
    if (display != NULL) {
        environment.width = (uint32_t)lv_display_get_horizontal_resolution(display);
        environment.height = (uint32_t)lv_display_get_vertical_resolution(display);
    }
    if (safe_insets != NULL) {
        environment.safe_insets[0] = safe_insets->top;
        environment.safe_insets[1] = safe_insets->right;
        environment.safe_insets[2] = safe_insets->bottom;
        environment.safe_insets[3] = safe_insets->left;
    }
    environment.display_shape = display_shape;
    memcpy(environment.corner_radii, corner_radii,
           sizeof(environment.corner_radii));
    (void)pxa_ui_update_environment(g_host.activation.services.ui,
                                    g_host.activation.ui_component,
                                    &environment);
    drain_active_events();
}

static void update_ui_resource_pressure(void) {
    size_t total;
    size_t available;
    size_t largest;
    pxa_ui_pressure_t pressure = PXA_UI_PRESSURE_NORMAL;
    if (g_host.activation.services.ui == NULL ||
        g_host.activation.ui_component == PXA_COMPONENT_INVALID) return;
    total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (total == 0) return;
    if (available < total / 16u ||
        largest < PXA_ESP_HOST_UI_TRANSACTION_BYTES / 4u)
        pressure = PXA_UI_PRESSURE_CRITICAL;
    else if (available < total / 8u ||
             largest < PXA_ESP_HOST_UI_TRANSACTION_BYTES)
        pressure = PXA_UI_PRESSURE_CONSTRAINED;
    if (pxa_ui_set_pressure(g_host.activation.services.ui, g_host.activation.ui_component, pressure) ==
        PXA_STATUS_OK) drain_active_events();
}

static void post_tick(uint8_t slot, uint32_t generation,
                      uint64_t timestamp_us) {
    uint8_t message[12 + 8];
    uint8_t payload[8];
    pxa_writer_t writer;
    uint32_t component_value;
    pxa_component_t component;
    portENTER_CRITICAL(&g_clock_slots_lock);
    if (!pxa_host_clock_slots_consume(&g_host.clock_slots, slot, generation,
                                      &component_value)) {
        portEXIT_CRITICAL(&g_clock_slots_lock);
        return;
    }
    portEXIT_CRITICAL(&g_clock_slots_lock);
    component = (pxa_component_t)component_value;
    pxa_write_u64(payload, timestamp_us);
    pxa_writer_init(&writer, message, sizeof(message));
    if (pxa_writer_message(&writer, PXA_CLOCK_SERVICE_ID, PXA_CLOCK_TICK, 0,
                           payload, sizeof(payload)) == PXA_STATUS_OK) {
        (void)pxa_event_post(g_host.activation.runtime, component, message, writer.size,
                             0, 0x000400008001ULL);
        drain_active_events();
    }
}

static void stop_active_after_back(void) {
    if (g_host.activation.services.scheduler != NULL && g_host.activation.coordinator != NULL &&
        (pxa_scheduler_has_pending(g_host.activation.services.scheduler) || has_active_jobs())) {
        const pxa_component_t main_component = g_host.activation.active_component;
        (void)pxa_activation_deactivate(
            g_host.activation.coordinator, (pxa_bytes_t){(const uint8_t *)"main", 4},
            PXA_STOP_NORMAL);
        clear_component_timer_slots(main_component);
        untrack_active_component(main_component);
        pxa_lvgl_ui_reset(g_host.ui_adapter);
        g_host.activation.active_component = PXA_COMPONENT_INVALID;
        g_host.activation.active_instance_id = 0;
        publish_main_stopped();
        clear_pointer_mailbox();
        return;
    }
    stop_active(PXA_STOP_NORMAL);
}

/* Delivers queued events until the reliable Back request reaches the main
 * component, then applies the Window service's handled/default-close rule. */
static int dispatch_back_request(uint8_t *close_requested) {
    pxa_wamr_event_result_t result;
    pxa_message_view_t event;
    pxa_status_t status;
    uint8_t attempt;
    if (close_requested == NULL || g_host.activation.services.window == NULL ||
        g_host.activation.active_component == PXA_COMPONENT_INVALID) {
        return 0;
    }
    status = pxa_window_queue_back(g_host.activation.services.window, g_host.activation.active_component);
    if (status != PXA_STATUS_OK) return 0;
    for (attempt = 0; attempt < PXA_ESP_HOST_BACK_EVENT_DRAIN_LIMIT;
         ++attempt) {
        status = pxa_wamr_engine_deliver_event_result(
            g_host.engine, g_host.activation.runtime, g_host.activation.active_component, &result);
        if (status != PXA_STATUS_OK) {
            if (!event_delivery_deferred(status, &result)) {
                fault_active_component(g_host.activation.active_component, status);
                return -1;
            }
            /* Keep a queued Back request from closing the app when its
             * transient Guest buffer cannot be allocated yet. */
            if (status == PXA_STATUS_RESOURCE_LIMIT) return -1;
            return 0;
        }
        (void)pxa_ipc_flush(g_host.activation.services.ipc);
        if (result.service != PXA_WINDOW_SERVICE_ID ||
            result.opcode != PXA_WINDOW_BACK_REQUESTED) {
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.service = result.service;
        event.opcode = result.opcode;
        event.request_id = result.request_id;
        event.payload.size = result.payload_size;
        status = pxa_window_resolve_event_result(&event, result.guest_result,
                                                 close_requested);
        if (status != PXA_STATUS_OK) {
            fault_active_component(g_host.activation.active_component, status);
            return -1;
        }
        return 1;
    }
    ESP_LOGW(PXA_ESP_HOST_TAG, "Back event did not reach active component");
    return 0;
}

static void post_back(void) {
    uint8_t close_requested = 1;
    int dispatched;
    if (g_host.activation.active_component == PXA_COMPONENT_INVALID) return;
    dispatched = dispatch_back_request(&close_requested);
    if (dispatched < 0 || (dispatched > 0 && !close_requested)) return;
    stop_active_after_back();
}

static void apply_active_permission(uint16_t permission_index, bool granted) {
    pxa_status_t status;
    pxa_component_t affected[4];
    size_t count = 0;
    size_t index;
    if (g_host.activation.services.permission == NULL ||
        permission_index >= g_host.activation.services.declaration_count) {
        return;
    }
    if (granted) {
        status = pxa_permission_set(
            g_host.activation.services.permission, g_host.activation.services.declarations[permission_index].name,
            g_host.activation.services.declarations[permission_index].scope,
            PXA_PERMISSION_ALLOW);
    } else {
        status = pxa_permission_revoke(
            g_host.activation.services.permission, g_host.activation.services.declarations[permission_index].name,
            g_host.activation.services.declarations[permission_index].scope, affected,
            sizeof(affected) / sizeof(affected[0]), &count);
    }
    if (status != PXA_STATUS_OK) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Active permission update failed: index=%u status=%d",
                 (unsigned)permission_index, (int)status);
        (void)pxa_esp_package_store_refresh_permission_summary(
            g_host.activation.active_identity);
        pxa_esp_ui_shell_refresh_apps();
        return;
    }
    (void)pxa_esp_package_store_refresh_permission_summary(
        g_host.activation.active_identity);
    pxa_esp_ui_shell_refresh_apps();
    for (index = 0; index < count && index < sizeof(affected) / sizeof(affected[0]);
         ++index) {
        if (!drain_events(affected[index])) break;
    }
}

static void complete_runtime_permission_prompt(uint32_t prompt_id,
                                                bool granted) {
    pxa_esp_host_permission_prompt_t pending;
    pxa_status_t status;
    if (g_store_result.prompt_id == prompt_id) {
        char identity[PXA_ESP_PACKAGE_ID_BYTES];
        snprintf(identity, sizeof(identity), "%s", g_store_result.identity);
        pxa_esp_ui_shell_dismiss_store_result(prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        memset(&g_store_result, 0, sizeof(g_store_result));
        if (granted && identity[0] != '\0')
            (void)pxa_esp_host_launch(identity);
        return;
    }
    if (g_store_install != NULL && g_store_install->prompt_id == prompt_id) {
        pxa_esp_ui_shell_dismiss_permission_prompt(prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        g_store_install->prompt_id = 0;
        g_store_install->approved = granted &&
            g_host.activation.runtime != NULL &&
            g_store_install->instance_id == g_host.activation.active_instance_id &&
            pxa_request_commit(g_host.activation.runtime,
                               g_store_install->component,
                               g_store_install->request_id) == PXA_STATUS_OK;
        xTaskNotifyGive(g_store_install->worker);
        return;
    }
    if (g_store_uninstall.prompt_id == prompt_id) {
        const uint32_t component = g_store_uninstall.component;
        const uint32_t request_id = g_store_uninstall.request_id;
        const uint64_t instance_id = g_store_uninstall.instance_id;
        char identity[PXA_ESP_PACKAGE_ID_BYTES];
        char name[PXA_ESP_PACKAGE_NAME_BYTES];
        snprintf(identity, sizeof(identity), "%s", g_store_uninstall.identity);
        snprintf(name, sizeof(name), "%s", g_store_uninstall.name);
        pxa_esp_ui_shell_dismiss_permission_prompt(prompt_id);
        pxa_esp_surface_runtime_modal_leave();
        memset(&g_store_uninstall, 0, sizeof(g_store_uninstall));
        if (g_host.activation.runtime != NULL &&
            instance_id == g_host.activation.active_instance_id &&
            pxa_request_is_active(g_host.activation.runtime, component,
                                  request_id)) {
            status = PXA_STATUS_DENIED;
            if (granted) {
                status = pxa_request_commit(g_host.activation.runtime,
                                            component, request_id);
                if (status == PXA_STATUS_OK)
                    status = pxa_esp_package_store_uninstall(identity) ?
                             PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
            }
            (void)pxa_request_complete(g_host.activation.runtime, component,
                                       request_id, status, NULL, 0);
            if (status == PXA_STATUS_OK) {
                pxa_esp_ui_shell_refresh_apps();
                show_store_result(true, name, identity);
            }
        }
        return;
    }
    if (!g_host.activation.permission_prompt.active ||
        g_host.activation.permission_prompt.prompt_id != prompt_id ||
        g_host.activation.services.permission == NULL) {
        return;
    }
    pending = g_host.activation.permission_prompt;
    memset(&g_host.activation.permission_prompt, 0, sizeof(g_host.activation.permission_prompt));
    status = pxa_permission_prompt_complete(
        g_host.activation.services.permission, pending.component, pending.request_id,
        granted ? PXA_PERMISSION_ALLOW : PXA_PERMISSION_DENY);
    if (status == PXA_STATUS_OK) {
        (void)pxa_esp_package_store_refresh_permission_summary(
            g_host.activation.active_identity);
        if (drain_events(pending.component)) {
            ESP_LOGI(PXA_ESP_HOST_TAG,
                     "Runtime permission %s: prompt=%u",
                     granted ? "granted" : "denied", (unsigned)prompt_id);
        }
    } else if (status != PXA_STATUS_NOT_FOUND && status != PXA_STATUS_BAD_STATE) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Runtime permission completion failed: prompt=%u status=%d",
                 (unsigned)prompt_id, (int)status);
    }
    pxa_esp_surface_runtime_modal_leave();
}

static void handle_store_install_event(pxa_esp_store_job_t *job,
                                        uint8_t phase, uint64_t downloaded_bytes) {
    pxa_esp_ui_permission_prompt_t prompt = {0};
    if (job == NULL || job != g_store_install) return;
    if (phase == 4u) {
        uint8_t payload[20];
        if (g_host.activation.runtime == NULL ||
            job->instance_id != g_host.activation.active_instance_id ||
            !pxa_request_is_active(g_host.activation.runtime, job->component,
                                   job->request_id)) return;
        for (uint8_t index = 0; index < 4u; ++index)
            payload[index] = (uint8_t)(job->request_id >> (index * 8u));
        for (uint8_t index = 0; index < 8u; ++index) {
            payload[4u + index] = (uint8_t)(downloaded_bytes >> (index * 8u));
            payload[12u + index] = (uint8_t)(job->size >> (index * 8u));
        }
        (void)pxa_event_post_message(
            g_host.activation.runtime, job->component,
            PXA_STORE_INSTALL_SERVICE_ID, PXA_STORE_DOWNLOAD_PROGRESS, 0,
            (pxa_bytes_t){payload, sizeof(payload)}, 0,
            ((uint64_t)PXA_STORE_INSTALL_SERVICE_ID << 32) |
                PXA_STORE_DOWNLOAD_PROGRESS);
        return;
    }
    if (phase == 3u) {
        pxa_esp_store_download_entry_t *entry = store_download_free();
        bool saved = false;
        if (g_host.activation.runtime != NULL &&
            job->instance_id == g_host.activation.active_instance_id &&
            entry != NULL) {
            snprintf(entry->filename, sizeof(entry->filename), "%s", job->filename);
            snprintf(entry->app_id, sizeof(entry->app_id), "%s", job->app_id);
            snprintf(entry->owner, sizeof(entry->owner), "%s",
                     g_host.activation.active_identity);
            saved = store_registry_save();
        }
        if (!saved) {
            if (entry != NULL) {
                memset(entry, 0, sizeof(*entry));
                (void)store_registry_save();
            }
            char path[160];
            if (snprintf(path, sizeof(path), "%s/%s/%s", CONFIG_PXA_MOUNT_POINT,
                         CONFIG_PXA_STATE_ROOT, job->filename) < (int)sizeof(path))
                (void)unlink(path);
            job->status = PXA_STATUS_IO_ERROR;
        }
        return;
    }
    if (phase == 1u) {
        if (g_host.activation.runtime == NULL ||
            job->instance_id != g_host.activation.active_instance_id ||
            !pxa_request_is_active(g_host.activation.runtime, job->component,
                                   job->request_id) ||
            g_host.activation.permission_prompt.active) {
            job->approved = 0;
            xTaskNotifyGive(job->worker);
            return;
        }
        prompt.prompt_id = ++g_host.activation.next_permission_prompt_id;
        if (prompt.prompt_id == 0)
            prompt.prompt_id = ++g_host.activation.next_permission_prompt_id;
        prompt.is_install = 1u;
        copy_utf8_c_string(prompt.app_name, sizeof(prompt.app_name),
                           job->preview.name);
        const bool chinese = g_host.locale[0] == 'z' && g_host.locale[1] == 'h';
        snprintf(prompt.permission_name, sizeof(prompt.permission_name),
                 chinese ? "安装应用 %.36s (%.20s)？" :
                           "Install app %.36s (%.20s)?",
                 job->app_id, job->preview.version);
        const char *requester = strrchr(g_host.activation.active_identity, ':');
        requester = requester == NULL ? g_host.activation.active_identity
                                      : requester + 1;
        snprintf(prompt.scope, sizeof(prompt.scope),
                 chinese ? "请求方 %.24s；发布者 %.16s…；%u 项权限" :
                           "Requester %.24s; publisher %.16s...; %u permissions",
                 requester, job->preview.id, (unsigned)job->preview.permission_count);
        job->prompt_id = prompt.prompt_id;
        pxa_esp_surface_runtime_modal_enter();
        if (!pxa_esp_ui_shell_post_permission_prompt(&prompt)) {
            job->prompt_id = 0;
            pxa_esp_surface_runtime_modal_leave();
            job->approved = 0;
            xTaskNotifyGive(job->worker);
        }
        return;
    }
    if (job->prompt_id != 0) {
        pxa_esp_ui_shell_dismiss_permission_prompt(job->prompt_id);
        pxa_esp_surface_runtime_modal_leave();
    }
    g_store_install = NULL;
    if (g_host.activation.runtime != NULL &&
        job->instance_id == g_host.activation.active_instance_id &&
        pxa_request_is_active(g_host.activation.runtime, job->component,
                              job->request_id)) {
        (void)pxa_request_complete(g_host.activation.runtime, job->component,
                                   job->request_id, job->status,
                                   job->separate == 1u && job->status == PXA_STATUS_OK ?
                                       job->filename : NULL,
                                   job->separate == 1u && job->status == PXA_STATUS_OK ?
                                       strlen(job->filename) : 0u);
        if (job->separate != 1u && job->status == PXA_STATUS_OK)
            show_store_result(false, job->preview.name, job->preview.id);
    }
    free(job);
}

static void maintain_scheduler(void) {
    pxa_scheduler_entry_t *due = NULL;
    size_t due_count = 0;
    size_t index;
    pxa_status_t due_status;
    if (g_host.activation.services.scheduler == NULL ||
        g_host.activation.coordinator == NULL) {
        return;
    }
    maintain_jobs();
    due_status = pxa_scheduler_take_due(g_host.activation.services.scheduler,
                                        NULL, 0, &due_count);
    if (due_status != PXA_STATUS_OK || due_count != 0) {
        if (due_status != PXA_STATUS_RESOURCE_LIMIT || due_count == 0 ||
            due_count > SIZE_MAX / sizeof(*due) ||
            (due = (pxa_scheduler_entry_t *)esp_alloc(
                 due_count * sizeof(*due))) == NULL ||
            pxa_scheduler_take_due(g_host.activation.services.scheduler, due,
                                   due_count, &due_count) != PXA_STATUS_OK) {
            if (due_status != PXA_STATUS_OK &&
                due_status != PXA_STATUS_RESOURCE_LIMIT) {
                ESP_LOGW(PXA_ESP_HOST_TAG,
                         "Work due scan failed: status=%d", (int)due_status);
            } else if (due_count != 0) {
                ESP_LOGE(PXA_ESP_HOST_TAG,
                         "Work due entries could not be allocated or claimed: count=%u",
                         (unsigned)due_count);
            }
            free(due);
            return;
        }
    }
    for (index = 0; index < due_count; ++index) {
        uint8_t config[PXA_WAMR_ENGINE_MAX_CONFIG_BYTES];
        size_t config_size = 0;
        char component_id[65];
        size_t component_size = due[index].component_id_size;
        pxa_component_t job_component = PXA_COMPONENT_INVALID;
        uint64_t job_instance;
        int job_slot;
        pxa_status_t activation_status;
        if (component_size >= sizeof(component_id)) continue;
        memcpy(component_id, due[index].component_id, component_size);
        component_id[component_size] = '\0';
        job_slot = job_component_slot(
            (pxa_bytes_t){(const uint8_t *)component_id, component_size});
        if (job_slot < 0 ||
            g_host.activation.active_jobs[job_slot] !=
                PXA_COMPONENT_INVALID) {
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Work deferred: id=%u component=%s is unavailable",
                     (unsigned)due[index].id, component_id);
            defer_work(&due[index]);
            continue;
        }
        job_instance = allocate_instance_id();
        {
            const uint64_t now_ms = host_clock_ms(NULL);
            if (now_ms > UINT64_MAX - due[index].max_execution_ms ||
                pxa_scheduler_encode_start_config(
                    &due[index], now_ms + due[index].max_execution_ms, config,
                    sizeof(config), &config_size) != PXA_STATUS_OK) {
                ESP_LOGE(PXA_ESP_HOST_TAG,
                         "Work start config failed: id=%u component=%s",
                         (unsigned)due[index].id, component_id);
                defer_work(&due[index]);
                continue;
            }
            g_host.activation.job_stop_at_ms[job_slot] =
                now_ms + due[index].max_execution_ms;
        }
        if (pxa_wamr_engine_set_config(
                g_host.engine, job_instance,
                (pxa_bytes_t){config, config_size}) != PXA_STATUS_OK) {
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Work deferred: id=%u component=%s configuration failed",
                     (unsigned)due[index].id, component_id);
            defer_work(&due[index]);
            continue;
        }
        g_host.activation.active_work[job_slot] = due[index];
        g_host.activation.work_result[job_slot] = 0;
        g_host.activation.activating_job_slot = (int16_t)job_slot;
        activation_status = activate_component_profiled(
            (pxa_bytes_t){(const uint8_t *)component_id, component_size},
            job_instance, &job_component);
        if (activation_status != PXA_STATUS_OK) {
            g_host.activation.activating_job_slot = -1;
            g_host.activation.job_stop_at_ms[job_slot] = 0;
            memset(&g_host.activation.active_work[job_slot], 0,
                   sizeof(g_host.activation.active_work[job_slot]));
            g_host.activation.work_result[job_slot] = 0;
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Work deferred: id=%u component=%s activation status=%d",
                     (unsigned)due[index].id, component_id,
                     (int)activation_status);
            defer_work(&due[index]);
            continue;
        }
        g_host.activation.activating_job_slot = -1;
        if (!track_active_component(job_component)) {
            (void)pxa_activation_deactivate(
                g_host.activation.coordinator,
                (pxa_bytes_t){(const uint8_t *)component_id, component_size},
                PXA_STOP_NORMAL);
            g_host.activation.job_stop_at_ms[job_slot] = 0;
            memset(&g_host.activation.active_work[job_slot], 0,
                   sizeof(g_host.activation.active_work[job_slot]));
            g_host.activation.work_result[job_slot] = 0;
            ESP_LOGW(PXA_ESP_HOST_TAG,
                     "Work deferred: id=%u component=%s tracking failed",
                     (unsigned)due[index].id, component_id);
            defer_work(&due[index]);
            continue;
        }
        g_host.activation.active_jobs[job_slot] = job_component;
        ESP_LOGI(PXA_ESP_HOST_TAG,
                 "Work started: id=%u component=%s attempt=%u",
                 (unsigned)due[index].id, component_id,
                 (unsigned)due[index].attempt);
    }
    free(due);
    drain_active_events();
    maintain_jobs();
    if (g_host.activation.active_component == PXA_COMPONENT_INVALID &&
        !has_active_jobs() &&
        !pxa_scheduler_has_pending(g_host.activation.services.scheduler)) {
        stop_active(PXA_STOP_NORMAL);
    }
}

static void run(void) {
    pxa_esp_host_command_t command;
    for (;;) {
        int handled = 0;
        pxa_ui_color_scheme_t color_scheme;
        pxa_window_insets_t pending_safe_insets;
        pxa_window_insets_t pending_system_bar_insets;
        if (take_pending_color_scheme(&color_scheme)) {
            apply_color_scheme(color_scheme);
            handled = 1;
        }
        uint32_t rgba[PXA_UI_THEME_ROLE_COUNT];
        if (take_pending_ui_palette(rgba)) {
            apply_ui_palette(rgba);
            handled = 1;
        }
        if (take_pending_window_insets(&pending_safe_insets,
                                       &pending_system_bar_insets)) {
            apply_window_insets(&pending_safe_insets,
                                &pending_system_bar_insets);
            handled = 1;
        }
        if (g_host.activation.services.surface != NULL) {
            int32_t released = pxa_surface_service_flush_releases(
                g_host.activation.services.surface);
            if (released > 0) {
                drain_active_events();
                handled = 1;
            }
        }
        /* A pointer move can run guest code and build a full Canvas frame.
         * Handle only one here so a continuous drag cannot starve ticks or
         * system BACK requests waiting in the regular command queue. */
        if (take_pointer_command(&command, host_now_us(NULL))) {
            post_pointer(&command);
            handled = 1;
        }
        if (take_net_completion_ready()) {
            drain_net_completions();
            handled = 1;
        }
        if (take_scheduler_maintenance_due()) {
            maintain_scheduler();
            handled = 1;
        }
        if (xQueueReceive(g_host.queue, &command, 0) == pdTRUE) {
            handled = 1;
            switch (command.type) {
            case PXA_ESP_HOST_CMD_START:
                ESP_LOGI(PXA_ESP_HOST_TAG, "Processing launch request: %s",
                         command.payload.identity.identity);
                if (!start_verified(command.payload.identity.identity)) {
                    ESP_LOGW(PXA_ESP_HOST_TAG, "Package launch failed: %s",
                             command.payload.identity.identity);
                    notify_runtime_event(PXA_HOST_RUNTIME_START_FAILED,
                                         command.payload.identity.identity);
                }
                break;
            case PXA_ESP_HOST_CMD_BACK:
                if (command.payload.instance.instance_id != 0 &&
                    command.payload.instance.instance_id ==
                        g_host.activation.active_instance_id) {
                    post_back();
                }
                break;
            case PXA_ESP_HOST_CMD_POINTER:
                post_pointer(&command);
                break;
            case PXA_ESP_HOST_CMD_UI_EVENT:
                post_ui_event(&command);
                break;
            case PXA_ESP_HOST_CMD_CONTROLLER:
                post_controller(&command);
                break;
            case PXA_ESP_HOST_CMD_STOP:
                if (g_host.activation.active_identity[0] != '\0' &&
                    strcmp(g_host.activation.active_identity,
                           command.payload.identity.identity) == 0) {
                    stop_active(PXA_STOP_NORMAL);
                }
                break;
            case PXA_ESP_HOST_CMD_SYSTEM_COMPLETE:
                complete_system_request(
                    command.payload.system_complete.response);
                break;
            case PXA_ESP_HOST_CMD_SYSTEM_EVENT:
                post_system_event(command.payload.system_event.event);
                break;
            case PXA_ESP_HOST_CMD_STORE_INSTALL:
                handle_store_install_event(command.payload.store_install.job,
                                           command.payload.store_install.phase,
                                           command.payload.store_install.downloaded_bytes);
                break;
            case PXA_ESP_HOST_CMD_TICK:
                /* The guest measures frame delta from delivery time. A tick
                 * held behind other work must not look like an on-time frame. */
                post_tick(command.payload.tick.slot,
                          command.payload.tick.generation,
                          host_now_us(NULL));
                break;
            case PXA_ESP_HOST_CMD_DRAIN:
                if (g_host.activation.active_component != PXA_COMPONENT_INVALID) {
                    drain_active_events();
                }
                break;
            case PXA_ESP_HOST_CMD_LEASE_MAINTENANCE: {
                pxa_component_t affected[2];
                size_t count = 0;
                if (g_host.activation.services.lease != NULL &&
                    pxa_lease_revoke_expired(
                        g_host.activation.services.lease, affected, 2, &count) ==
                        PXA_STATUS_OK) {
                    size_t index;
                    for (index = 0; index < count; ++index) {
                        if (!drain_events(affected[index])) break;
                    }
                }
                break;
            }
            case PXA_ESP_HOST_CMD_SENSOR_MAINTENANCE:
                break;
            case PXA_ESP_HOST_CMD_NET_MAINTENANCE: {
                drain_net_completions();
                break;
            }
            case PXA_ESP_HOST_CMD_WINDOW_MAINTENANCE:
                if (g_host.activation.active_component != PXA_COMPONENT_INVALID) {
                    update_window_snapshot(g_host.activation.active_component);
                    update_ui_resource_pressure();
                    drain_active_events();
                }
                break;
            case PXA_ESP_HOST_CMD_PERMISSION_DECISION:
                complete_runtime_permission_prompt(
                    command.payload.permission_decision.prompt_id,
                    command.payload.permission_decision.granted != 0);
                break;
            case PXA_ESP_HOST_CMD_PERMISSION_SET:
                if (g_host.activation.active_identity[0] != '\0' &&
                    strcmp(g_host.activation.active_identity,
                           command.payload.permission_set.identity) == 0) {
                    apply_active_permission(
                        command.payload.permission_set.permission_index,
                        command.payload.permission_set.granted != 0);
                }
                break;
            case PXA_ESP_HOST_CMD_UNRESPONSIVE_STOP:
                if (g_host.activation.active_identity[0] != '\0' &&
                    strcmp(g_host.activation.active_identity,
                           command.payload.identity.identity) == 0) {
                    stop_active(PXA_STOP_NORMAL);
                }
                break;
            case PXA_ESP_HOST_CMD_SCHEDULER_MAINTENANCE:
                maintain_scheduler();
                break;
            default:
                break;
            }
        }
        if (handled) {
            /* Timers can keep the queue nonempty indefinitely while a game is
             * rendering. Block for one scheduler tick so the lower-priority
             * IDLE task runs and services the system task watchdog. */
            vTaskDelay(1);
        } else {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

static void *thread_main(void *arg) {
    (void)arg;
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.runtime_task = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&g_process_state_lock);
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Runtime task started: core=%d affinity=%d priority=%d stack=%u bytes",
             (int)xPortGetCoreID(), (int)CONFIG_PXA_RUNTIME_TASK_AFFINITY,
             (int)CONFIG_PXA_RUNTIME_TASK_PRIORITY,
             (unsigned)runtime_task_stack_size());
    /* The catalog scan reads flash and therefore needs an internal-RAM stack.
     * Reuse the runtime task's stack during bootstrap instead of allocating a
     * second 10 KiB task after SRAM has already become fragmented. Launch
     * requests may queue concurrently and are processed once this returns. */
    pxa_esp_package_store_sync_builtins();
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Built-in Package bootstrap stack: min_free=%u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) *
                        sizeof(StackType_t)));
    run();
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.runtime_task = NULL;
    portEXIT_CRITICAL(&g_process_state_lock);
    return NULL;
}

static int start_runtime_thread(void) {
    esp_pthread_cfg_t previous = esp_pthread_get_default_config();
    esp_pthread_cfg_t config = esp_pthread_get_default_config();
    esp_err_t config_status;
    int create_status = -1;

    /* esp_pthread_get_cfg() returns NOT_FOUND when this task has no override;
     * in that case `previous` already contains the effective defaults. */
    (void)esp_pthread_get_cfg(&previous);
    config.stack_size = runtime_task_stack_size();
    config.prio = CONFIG_PXA_RUNTIME_TASK_PRIORITY;
    config.inherit_cfg = false;
    config.thread_name = "pxa_runtime";
    config.pin_to_core = CONFIG_PXA_RUNTIME_TASK_AFFINITY;
    config.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    config_status = esp_pthread_set_cfg(&config);
    if (config_status == ESP_OK) {
        create_status = pthread_create(&g_host.thread, NULL, thread_main, NULL);
    }
    (void)esp_pthread_set_cfg(&previous);
    if (config_status != ESP_OK || create_status != 0) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Runtime thread creation failed: config=0x%x pthread=%d",
                 (unsigned)config_status, create_status);
        return 0;
    }
    (void)pthread_detach(g_host.thread);
    return 1;
}

static int start_runtime_services(void) {
    if (!g_host.initialized || g_host.queue == NULL) return 0;
    portENTER_CRITICAL(&g_process_state_lock);
    if (g_host.runtime_started) {
        portEXIT_CRITICAL(&g_process_state_lock);
        return 1;
    }
    if (g_host.runtime_starting) {
        portEXIT_CRITICAL(&g_process_state_lock);
        return 0;
    }
    g_host.runtime_starting = 1;
    portEXIT_CRITICAL(&g_process_state_lock);

    if (esp_timer_start_periodic(g_host.clock_timer,
                                 PXA_ESP_HOST_CLOCK_PERIOD_US) != ESP_OK) {
        ESP_LOGE(PXA_ESP_HOST_TAG, "Clock timer start failed");
        portENTER_CRITICAL(&g_process_state_lock);
        g_host.runtime_starting = 0;
        portEXIT_CRITICAL(&g_process_state_lock);
        return 0;
    }
    if (esp_timer_start_periodic(g_host.watchdog_timer,
                                 PXA_ESP_HOST_WATCHDOG_PERIOD_US) != ESP_OK) {
        ESP_LOGE(PXA_ESP_HOST_TAG, "Watchdog timer start failed");
        (void)esp_timer_stop(g_host.clock_timer);
        portENTER_CRITICAL(&g_process_state_lock);
        g_host.runtime_starting = 0;
        portEXIT_CRITICAL(&g_process_state_lock);
        return 0;
    }
    if (!start_runtime_thread()) {
        (void)esp_timer_stop(g_host.watchdog_timer);
        (void)esp_timer_stop(g_host.clock_timer);
        portENTER_CRITICAL(&g_process_state_lock);
        g_host.runtime_starting = 0;
        portEXIT_CRITICAL(&g_process_state_lock);
        return 0;
    }
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.runtime_started = 1;
    g_host.runtime_starting = 0;
    portEXIT_CRITICAL(&g_process_state_lock);
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Runtime started (internal stack=%u bytes)",
             (unsigned)runtime_task_stack_size());
    return 1;
}

/* --- engine prepare_start: bind host-owned UI services ------------------- */

static pxa_status_t set_ui_startup_config(
    uint64_t instance_id, pxa_component_t component) {
    uint8_t config[PXA_WAMR_ENGINE_MAX_CONFIG_BYTES];
    uint8_t payload[8];
    uint8_t environment_bytes[128];
    uint8_t system_environment[4u + PXA_SYSTEM_LOCALE_MAX_BYTES + 5u];
    char locale[PXA_SYSTEM_LOCALE_MAX_BYTES + 1u];
    uint8_t locale_size;
    uint8_t text_direction;
    size_t environment_size;
    pxa_ui_environment_t environment;
    pxa_writer_t writer;
    pxa_writer_t system_writer;
    pxa_status_t status = pxa_ui_get_environment(
        g_host.activation.services.ui, component, PXA_UI_PRIMARY_SURFACE, &environment);
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
    portENTER_CRITICAL(&g_process_state_lock);
    locale_size = g_host.locale_size;
    text_direction = g_host.text_direction;
    memcpy(locale, g_host.locale, (size_t)locale_size + 1u);
    portEXIT_CRITICAL(&g_process_state_lock);
    pxa_writer_init(&system_writer, system_environment,
                    sizeof(system_environment));
    if (pxa_writer_record(&system_writer, PXA_SYSTEM_CONFIGURATION_LOCALE,
                          locale, locale_size) != PXA_STATUS_OK ||
        pxa_writer_record(&system_writer,
                          PXA_SYSTEM_CONFIGURATION_TEXT_DIRECTION,
                          &text_direction, 1u) != PXA_STATUS_OK ||
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

static pxa_status_t prepare_start(void *context, pxa_component_t component,
                                  uint64_t instance_id, uint8_t kind) {
    pxa_window_backend_t backend;
    pxa_status_t status;
    (void)context;
    if (kind != 1) return PXA_STATUS_OK;
    memset(&backend, 0, sizeof(backend));
    backend.struct_size = sizeof(backend);
    backend.apply = host_window_apply;
    backend.show_toast = host_window_toast;
    status = pxa_window_bind(g_host.activation.services.window, component, &backend);
    if (status != PXA_STATUS_OK) return status;
    status = pxa_ui_bind(g_host.activation.services.ui, component, &g_host.ui_backend);
    if (status != PXA_STATUS_OK) {
        (void)pxa_window_unbind(g_host.activation.services.window, component);
        return status;
    }
    {
        pxa_ui_environment_t environment;
        status = pxa_ui_get_environment(g_host.activation.services.ui, component,
                                        PXA_UI_PRIMARY_SURFACE, &environment);
        if (status == PXA_STATUS_OK &&
            g_host.ui_backend.environment_changed != NULL)
            status = g_host.ui_backend.environment_changed(
                g_host.ui_backend.context, &environment);
        if (status != PXA_STATUS_OK) {
            (void)pxa_ui_unbind(g_host.activation.services.ui, component);
            (void)pxa_window_unbind(g_host.activation.services.window, component);
            return status;
        }
    }
    status = set_ui_startup_config(instance_id, component);
    if (status != PXA_STATUS_OK) {
        (void)pxa_ui_unbind(g_host.activation.services.ui, component);
        (void)pxa_window_unbind(g_host.activation.services.window, component);
        return status;
    }
    g_host.activation.ui_component = component;
    return PXA_STATUS_OK;
}

/* --- public interface ----------------------------------------------------- */

/* Roll back only the process resources created before the runtime thread starts. */
static void rollback_host_initialization(void) {
    if (g_host.watchdog_timer != NULL) {
        (void)esp_timer_delete(g_host.watchdog_timer);
        g_host.watchdog_timer = NULL;
    }
    if (g_host.clock_timer != NULL) {
        (void)esp_timer_delete(g_host.clock_timer);
        g_host.clock_timer = NULL;
    }
    pxa_esp_audio_deinitialize();
    if (g_host.queue != NULL) {
        vQueueDelete(g_host.queue);
        g_host.queue = NULL;
    }
    pxa_esp_ui_assets_clear();
    if (g_host.ui_adapter != NULL) {
        pxa_lvgl_ui_deinit(g_host.ui_adapter);
        g_host.ui_adapter = NULL;
    }
    release_ui_fonts();
    if (g_host.engine != NULL) {
        pxa_wamr_engine_set_runtime(g_host.engine, NULL);
        pxa_wamr_engine_deinit(g_host.engine);
        g_host.engine = NULL;
    }
    if (g_host.activation.runtime != NULL) {
        pxa_runtime_deinit(g_host.activation.runtime);
        g_host.activation.runtime = NULL;
    }
    activation_memory_release_all();
    free(g_host.ui_adapter_workspace);
    free(g_host.engine_workspace);
    free(g_host.runtime_workspace);
    memset(&g_host, 0, sizeof(g_host));
}

bool pxa_esp_host_initialize(void) {
    pxa_lvgl_ui_config_t ui_config;
    size_t workspace_size;
    esp_timer_create_args_t timer_args;
    const char *failed_stage = NULL;
    if (g_host.initialized) return true;
    memcpy(g_host.locale, "en-US", sizeof("en-US"));
    g_host.locale_size = (uint8_t)(sizeof("en-US") - 1u);
    g_host.text_direction = 0;
    g_host.activation.activating_job_slot = -1;
    g_host.work_epoch = ((uint64_t)esp_random() << 32) | esp_random();
    if (g_host.work_epoch == 0) g_host.work_epoch = 1;
    if (!pxa_host_activation_arena_init(
            &g_host.activation.activation_memory, NULL,
            esp_activation_alloc,
            esp_activation_free)) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Activation arena initialization failed");
        return false;
    }
    if (!pxa_esp_package_store_initialize()) {
        ESP_LOGE(PXA_ESP_HOST_TAG, "Package store initialization failed");
        return false;
    }
    store_registry_load();
    pxa_esp_store_cleanup_interrupted(store_registry_keep, NULL);
    (void)store_registry_save();

    /* Retained LVGL UI backend. */
    memset(&ui_config, 0, sizeof(ui_config));
    ui_config.struct_size = sizeof(ui_config);
    ui_config.allocator_context = NULL;
    ui_config.allocate = esp_ui_alloc;
    ui_config.release = esp_ui_free;
    ui_config.execute = esp_lvgl_execute;
    pxa_lvgl_ui_theme_init(&ui_config.theme);
    g_host.ui_caption_font = load_ui_font(12, pxa_esp_ui_shell_text_font());
    g_host.ui_label_font = load_ui_font(14, pxa_esp_ui_shell_text_font());
    g_host.ui_body_font = load_ui_font(16, pxa_esp_ui_shell_text_font());
    g_host.ui_title_font = load_ui_font(20, pxa_esp_ui_shell_title_font());
    g_host.ui_headline_font = load_ui_font(24, pxa_esp_ui_shell_title_font());
    g_host.ui_display_font = load_ui_font(28, pxa_esp_ui_shell_title_font());
    ui_config.theme.caption_font = g_host.ui_caption_font != NULL
                                       ? g_host.ui_caption_font
                                       : pxa_esp_ui_shell_text_font();
    ui_config.theme.label_font = g_host.ui_label_font != NULL
                                     ? g_host.ui_label_font
                                     : ui_config.theme.caption_font;
    ui_config.theme.body_font = g_host.ui_body_font != NULL
                                    ? g_host.ui_body_font
                                    : pxa_esp_ui_shell_text_font();
    ui_config.theme.title_font = g_host.ui_title_font != NULL
                                     ? g_host.ui_title_font
                                     : pxa_esp_ui_shell_title_font();
    ui_config.theme.headline_font = g_host.ui_headline_font != NULL
                                        ? g_host.ui_headline_font
                                        : ui_config.theme.title_font;
    ui_config.theme.display_font = g_host.ui_display_font != NULL
                                       ? g_host.ui_display_font
                                       : ui_config.theme.title_font;
    ui_config.theme.icon_font = pxa_esp_ui_shell_icon_font();
    g_host.ui_theme = ui_config.theme;
    g_host.ui_theme_generation = 1u;
    ui_config.resolve_asset = pxa_esp_ui_asset_resolve;
    ui_config.release_asset = pxa_esp_ui_asset_release;
    ui_config.event_callback = on_ui_event;
    ui_config.now_us = host_now_us;
    ui_config.primary_environment.surface = PXA_UI_PRIMARY_SURFACE;
    {
        lv_display_t *display;
        lv_lock();
        display = lv_display_get_default();
        if (display != NULL) {
            ui_config.primary_environment.width =
                (uint32_t)lv_display_get_horizontal_resolution(display);
            ui_config.primary_environment.height =
                (uint32_t)lv_display_get_vertical_resolution(display);
        }
        lv_unlock();
    }
    portENTER_CRITICAL(&g_process_state_lock);
    if (g_host.window_insets_valid) {
        ui_config.primary_environment.safe_insets[0] = g_host.safe_insets.top;
        ui_config.primary_environment.safe_insets[1] = g_host.safe_insets.right;
        ui_config.primary_environment.safe_insets[2] = g_host.safe_insets.bottom;
        ui_config.primary_environment.safe_insets[3] = g_host.safe_insets.left;
    }
    portEXIT_CRITICAL(&g_process_state_lock);
    ui_config.primary_environment.density_q16 = UINT32_C(1) << 16;
    ui_config.primary_environment.font_scale_q16 = UINT32_C(1) << 16;
    ui_config.primary_environment.color_scheme = g_host.color_scheme;
    ui_config.primary_environment.features =
        PXA_UI_FEATURE_CANVAS | PXA_UI_FEATURE_VIRTUAL_LIST |
        PXA_UI_FEATURE_GRID | PXA_UI_FEATURE_RGB565_BITMAP |
        PXA_UI_FEATURE_CONTROLLER_INPUT;
    workspace_size = pxa_lvgl_ui_workspace_size();
    if (workspace_size == 0) {
        failed_stage = "size-lvgl-workspace";
        goto failed;
    }
    g_host.ui_adapter_workspace = esp_alloc(workspace_size);
    if (g_host.ui_adapter_workspace == NULL) {
        failed_stage = "allocate-lvgl-workspace";
        goto failed;
    }
    if (pxa_lvgl_ui_init(g_host.ui_adapter_workspace, workspace_size,
                            &ui_config, &g_host.ui_adapter,
                            &g_host.ui_backend) != PXA_STATUS_OK) {
        failed_stage = "initialize-lvgl-adapter";
        goto failed;
    }
    pxa_esp_surface_set_ui_alpha_provider(provide_ui_alpha_plane, &g_host);
    pxa_esp_surface_set_release_ready_callback(on_surface_release_ready, NULL);
    /* Command queue. */
    g_host.queue = xQueueCreateWithCaps(
        CONFIG_PXA_COMMAND_QUEUE_LENGTH, sizeof(pxa_esp_host_command_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_host.queue == NULL) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "PSRAM command queue allocation failed; using internal SRAM");
        g_host.queue = xQueueCreateWithCaps(
            CONFIG_PXA_COMMAND_QUEUE_LENGTH, sizeof(pxa_esp_host_command_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (g_host.queue == NULL) {
        failed_stage = "allocate-command-queue";
        goto failed;
    }
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Command queue ready in %s (%u x %u bytes); pointer mailbox %u x %u bytes",
             esp_ptr_external_ram(g_host.queue) ? "PSRAM" : "internal SRAM",
             (unsigned)CONFIG_PXA_COMMAND_QUEUE_LENGTH,
             (unsigned)sizeof(pxa_esp_host_command_t),
             (unsigned)PXA_HOST_POINTER_MAILBOX_CAPACITY,
             (unsigned)sizeof(g_host.pointer_mailbox));
    /* Timers are created now and started with the deferred runtime thread. */
    memset(&timer_args, 0, sizeof(timer_args));
    timer_args.callback = on_clock_timer;
    timer_args.name = "pxa_clock";
    timer_args.skip_unhandled_events = true;
    if (esp_timer_create(&timer_args, &g_host.clock_timer) != ESP_OK) {
        failed_stage = "create-clock-timer";
        goto failed;
    }
    memset(&timer_args, 0, sizeof(timer_args));
    timer_args.callback = on_watchdog_timer;
    timer_args.name = "pxa_call";
    timer_args.skip_unhandled_events = true;
    if (esp_timer_create(&timer_args, &g_host.watchdog_timer) != ESP_OK) {
        failed_stage = "create-watchdog-timer";
        goto failed;
    }
    pxa_esp_ui_shell_bind();
    g_host.initialized = 1;
    ESP_LOGI(PXA_ESP_HOST_TAG,
             "Host initialized; built-in sync waits for runtime start (stack=%u bytes)",
             (unsigned)runtime_task_stack_size());
    return true;

failed:
    ESP_LOGE(PXA_ESP_HOST_TAG, "Host initialization failed at %s",
             failed_stage == NULL ? "unknown" : failed_stage);
    rollback_host_initialization();
    return false;
}

bool pxa_esp_host_start_runtime(void) {
    return start_runtime_services() != 0;
}

void pxa_esp_host_sync_builtins(void) {
    pxa_esp_package_store_sync_builtins();
}

bool pxa_esp_host_refresh_inbox(void) {
    return g_host.initialized && pxa_esp_package_store_refresh_inbox();
}

bool pxa_esp_host_stage_package_file(const char *source_path) {
    return g_host.initialized &&
           pxa_esp_package_store_stage_file(source_path);
}

bool pxa_esp_host_install_package_file(const char *source_path) {
    return g_host.initialized &&
           pxa_esp_package_store_install_file(source_path);
}

bool pxa_esp_host_launch(const char *identity) {
    pxa_esp_host_command_t command;
    if (!g_host.initialized || identity == NULL) return false;
    if (!start_runtime_services()) {
        ESP_LOGE(PXA_ESP_HOST_TAG,
                 "Unable to start runtime for Package %s", identity);
        return false;
    }
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_START;
    snprintf(command.payload.identity.identity,
             sizeof(command.payload.identity.identity), "%s", identity);
    ESP_LOGI(PXA_ESP_HOST_TAG, "Queueing launch request: %s", identity);
    if (!post_command(&command)) {
        ESP_LOGW(PXA_ESP_HOST_TAG,
                 "Launch queue full; request rejected: %s", identity);
        return false;
    }
    return true;
}

bool pxa_esp_host_back(void) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized) return false;
    public_state_snapshot(&public_state);
    if (!public_state.main_active) return false;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_BACK;
    command.payload.instance.instance_id = public_state.main_instance_id;
    return post_command(&command) != 0;
}

bool pxa_esp_host_stop(const char *identity) {
    pxa_esp_host_command_t command;
    if (!g_host.initialized || identity == NULL || identity[0] == '\0')
        return false;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_STOP;
    snprintf(command.payload.identity.identity,
             sizeof(command.payload.identity.identity), "%s", identity);
    return post_command(&command) != 0;
}

bool pxa_esp_host_is_active(const char *identity) {
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized || identity == NULL) return false;
    public_state_snapshot(&public_state);
    return public_state.package_active &&
           strcmp(public_state.identity, identity) == 0;
}

bool pxa_esp_host_captures_volume_keys(void) {
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized) return false;
    public_state_snapshot(&public_state);
    return public_state.main_active &&
           public_state.volume_key_capture_active != 0;
}

bool pxa_esp_host_post_key(uint16_t key) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized) return false;
    public_state_snapshot(&public_state);
    if (!public_state.main_active || public_state.main_instance_id == 0) {
        return false;
    }
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_UI_EVENT;
    command.payload.ui_event.surface = PXA_UI_PRIMARY_SURFACE;
    command.payload.ui_event.node = 1u;
    command.payload.ui_event.value = (int32_t)key;
    command.payload.ui_event.kind = PXA_UI_EVENT_KEY;
    command.payload.ui_event.flags = PXA_UI_EVENT_FLAG_RELIABLE;
    command.payload.ui_event.instance_id = public_state.main_instance_id;
    command.payload.ui_event.timestamp_us = host_now_us(NULL);
    return post_command(&command) != 0;
}

bool pxa_esp_host_post_controller_state(uint8_t controller, bool connected,
                                        uint32_t buttons) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized || (!connected && buttons != 0) ||
        (buttons & ~PXA_UI_CONTROLLER_BUTTON_MASK) != 0)
        return false;
    public_state_snapshot(&public_state);
    if (!public_state.main_active || public_state.main_instance_id == 0)
        return false;
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_CONTROLLER;
    command.payload.controller.instance_id = public_state.main_instance_id;
    command.payload.controller.timestamp_us = host_now_us(NULL);
    command.payload.controller.controller = controller;
    command.payload.controller.connected = connected ? 1u : 0u;
    command.payload.controller.buttons = buttons;
    return post_command(&command) != 0;
}

bool pxa_esp_host_active_identity(char *identity, size_t capacity) {
    pxa_esp_host_public_state_t public_state;
    size_t identity_size;
    if (!g_host.initialized || identity == NULL || capacity == 0) return false;
    public_state_snapshot(&public_state);
    if (!public_state.package_active || !public_state.main_active) {
        identity[0] = '\0';
        return false;
    }
    identity_size = strlen(public_state.identity);
    if (identity_size >= capacity) {
        identity[0] = '\0';
        return false;
    }
    memcpy(identity, public_state.identity, identity_size + 1u);
    return true;
}

void pxa_esp_host_set_runtime_event_callback(
    pxa_host_runtime_event_fn callback, void *context) {
    portENTER_CRITICAL(&g_runtime_event_lock);
    g_runtime_event_callback = callback;
    g_runtime_event_context = context;
    portEXIT_CRITICAL(&g_runtime_event_lock);
}

void pxa_esp_host_set_system_request_callback(
    pxa_host_system_request_fn callback, void *context) {
    portENTER_CRITICAL(&g_system_request_lock);
    g_system_request_callback = callback;
    g_system_request_context = context;
    portEXIT_CRITICAL(&g_system_request_lock);
}

void pxa_esp_host_set_window_changed_callback(
    pxa_host_window_changed_fn callback, void *context) {
    portENTER_CRITICAL(&g_window_changed_lock);
    g_window_changed_callback = callback;
    g_window_changed_context = context;
    portEXIT_CRITICAL(&g_window_changed_lock);
}

bool pxa_esp_host_complete_system_request(const char *app_id,
                                          uint32_t component,
                                          uint32_t request_id, int32_t status,
                                          const void *payload,
                                          size_t payload_size) {
    pxa_host_system_response_t *response;
    pxa_esp_host_command_t command;
    size_t app_id_size;
    if (!g_host.initialized || app_id == NULL || component == 0 ||
        request_id == 0 || (payload == NULL && payload_size != 0) ||
        payload_size > PXA_MAX_CONTROL_MESSAGE - 16u ||
        payload_size > SIZE_MAX - sizeof(*response)) {
        return false;
    }
    app_id_size = strlen(app_id);
    if (app_id_size == 0 || app_id_size >= sizeof(response->app_id))
        return false;
    response = malloc(sizeof(*response) + payload_size);
    if (response == NULL) return false;
    memset(response, 0, sizeof(*response));
    memcpy(response->app_id, app_id, app_id_size + 1u);
    response->component = (pxa_component_t)component;
    response->request_id = request_id;
    response->status = pxa_status_is_known((pxa_status_t)status)
                           ? (pxa_status_t)status
                           : PXA_STATUS_INTERNAL;
    response->payload_size = payload_size;
    if (payload_size != 0) memcpy(response->payload, payload, payload_size);
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_SYSTEM_COMPLETE;
    command.payload.system_complete.response = response;
    if (!post_command(&command)) {
        free(response);
        return false;
    }
    return true;
}

bool pxa_esp_host_post_system_event(const char *app_id, uint32_t component,
                                    uint16_t opcode, uint32_t request_id,
                                    const void *payload,
                                    size_t payload_size) {
    pxa_host_system_event_t *event;
    pxa_esp_host_command_t command;
    size_t app_id_size;
    if (!g_host.initialized || app_id == NULL || component == 0 ||
        opcode < UINT16_C(0x8000) || (payload == NULL && payload_size != 0) ||
        payload_size > PXA_MAX_CONTROL_MESSAGE - 12u ||
        payload_size > SIZE_MAX - sizeof(*event)) {
        return false;
    }
    app_id_size = strlen(app_id);
    if (app_id_size == 0 || app_id_size >= sizeof(event->app_id))
        return false;
    event = malloc(sizeof(*event) + payload_size);
    if (event == NULL) return false;
    memset(event, 0, sizeof(*event));
    memcpy(event->app_id, app_id, app_id_size + 1u);
    event->component = (pxa_component_t)component;
    event->request_id = request_id;
    event->opcode = opcode;
    event->payload_size = payload_size;
    if (payload_size != 0) memcpy(event->payload, payload, payload_size);
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_SYSTEM_EVENT;
    command.payload.system_event.event = event;
    if (!post_command(&command)) {
        free(event);
        return false;
    }
    return true;
}

bool pxa_esp_host_post_app_system_event(const char *identity_key,
                                        uint16_t opcode, const void *payload,
                                        size_t payload_size) {
    pxa_host_system_event_t *event;
    pxa_esp_host_command_t command;
    size_t identity_size;
    if (!g_host.initialized || identity_key == NULL ||
        opcode < UINT16_C(0x8000) ||
        (payload == NULL && payload_size != 0) ||
        payload_size > PXA_MAX_CONTROL_MESSAGE - 12u ||
        payload_size > SIZE_MAX - sizeof(*event)) {
        return false;
    }
    identity_size = strlen(identity_key);
    if (identity_size == 0 || identity_size >= sizeof(event->app_id))
        return false;
    event = malloc(sizeof(*event) + payload_size);
    if (event == NULL) return false;
    memset(event, 0, sizeof(*event));
    memcpy(event->app_id, identity_key, identity_size + 1u);
    event->component = PXA_COMPONENT_INVALID;
    event->opcode = opcode;
    event->payload_size = payload_size;
    if (payload_size != 0) memcpy(event->payload, payload, payload_size);
    memset(&command, 0, sizeof(command));
    command.type = (uint8_t)PXA_ESP_HOST_CMD_SYSTEM_EVENT;
    command.payload.system_event.event = event;
    if (!post_command(&command)) {
        free(event);
        return false;
    }
    return true;
}

bool pxa_esp_host_set_color_scheme(pxa_host_color_scheme_t color_scheme) {
    if (!g_host.initialized || color_scheme > PXA_HOST_COLOR_SCHEME_DARK)
        return false;
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.pending_color_scheme = (pxa_ui_color_scheme_t)color_scheme;
    g_host.color_scheme_pending = 1;
    portEXIT_CRITICAL(&g_process_state_lock);
    wake_runtime_thread();
    return true;
}

bool pxa_esp_host_set_ui_palette(const uint32_t rgba[10]) {
    if (!g_host.initialized || rgba == NULL) return false;
    uint32_t extended[PXA_UI_THEME_ROLE_COUNT];
    size_t index;
    memcpy(extended, rgba, 10u * sizeof(uint32_t));
    for (index = PXA_UI_THEME_COLOR_COUNT;
         index < PXA_UI_THEME_ROLE_COUNT; ++index)
        extended[index] = rgba[1];
    extended[16] = extended[18] = extended[20] = rgba[2];
    extended[22] = extended[24] = rgba[2];
    extended[17] = extended[19] = extended[23] = rgba[3];
    extended[21] = extended[25] = rgba[3];
    extended[15] = rgba[5];
    extended[26] = rgba[6];
    extended[27] = rgba[9];
    extended[28] = rgba[3];
    extended[30] = rgba[4];
    extended[31] = rgba[2];
    return pxa_esp_host_set_ui_palette_extended(extended);
}

bool pxa_esp_host_set_ui_palette_extended(
    const uint32_t rgba[PXA_UI_THEME_ROLE_COUNT]) {
    if (!g_host.initialized || rgba == NULL) return false;
    portENTER_CRITICAL(&g_process_state_lock);
    memcpy(g_host.pending_ui_palette, rgba, sizeof(g_host.pending_ui_palette));
    g_host.ui_palette_pending = 1;
    portEXIT_CRITICAL(&g_process_state_lock);
    wake_runtime_thread();
    return true;
}

bool pxa_esp_host_set_locale(const char *locale, uint8_t text_direction) {
    size_t size;
    if (!g_host.initialized || locale == NULL || text_direction > 1u) {
        return false;
    }
    size = strlen(locale);
    if (size < 2u || size > PXA_SYSTEM_LOCALE_MAX_BYTES) return false;
    portENTER_CRITICAL(&g_process_state_lock);
    memcpy(g_host.locale, locale, size + 1u);
    g_host.locale_size = (uint8_t)size;
    g_host.text_direction = text_direction;
    portEXIT_CRITICAL(&g_process_state_lock);
    return true;
}

bool pxa_esp_host_locale_is_chinese(void) {
    bool chinese;
    portENTER_CRITICAL(&g_process_state_lock);
    chinese = g_host.locale[0] == 'z' && g_host.locale[1] == 'h';
    portEXIT_CRITICAL(&g_process_state_lock);
    return chinese;
}

bool pxa_esp_host_set_window_insets(const pxa_window_insets_t *safe_insets,
                                    const pxa_window_insets_t *system_bar_insets) {
    if (!g_host.initialized ||
        (safe_insets == NULL && system_bar_insets == NULL))
        return false;
    portENTER_CRITICAL(&g_process_state_lock);
    if (safe_insets != NULL) g_host.pending_safe_insets = *safe_insets;
    if (system_bar_insets != NULL)
        g_host.pending_system_bar_insets = *system_bar_insets;
    g_host.window_insets_pending = 1;
    portEXIT_CRITICAL(&g_process_state_lock);
    wake_runtime_thread();
    return true;
}

bool pxa_esp_host_set_display_geometry(uint32_t shape, const uint16_t radii[4]) {
    if (!g_host.initialized || radii == NULL ||
        shape > PXA_UI_DISPLAY_SHAPE_CUSTOM)
        return false;
    portENTER_CRITICAL(&g_process_state_lock);
    g_host.display_shape = shape;
    for (uint8_t index = 0; index < 4; ++index)
        g_host.corner_radii[index] = radii[index];
    g_host.window_insets_pending = 1;
    portEXIT_CRITICAL(&g_process_state_lock);
    wake_runtime_thread();
    return true;
}

typedef struct {
    pxa_host_package_info_t *packages;
    size_t capacity;
    size_t count;
} pxa_esp_host_package_projection_t;

static bool project_host_package(const pxa_esp_package_record_t *record,
                                 void *user_data) {
    pxa_esp_host_package_projection_t *projection = user_data;
    pxa_host_package_info_t *package;
    if (record == NULL || projection == NULL ||
        projection->count >= projection->capacity) {
        return false;
    }
    package = &projection->packages[projection->count++];
    memset(package, 0, sizeof(*package));
    snprintf(package->id, sizeof(package->id), "%s", record->id);
    snprintf(package->app_id, sizeof(package->app_id), "%s", record->app_id);
    copy_utf8_c_string(package->name, sizeof(package->name), record->name);
    snprintf(package->version, sizeof(package->version), "%s",
             record->version);
    if (record->has_publisher_root) {
        memcpy(package->publisher_root, record->publisher_root,
               sizeof(package->publisher_root));
        package->has_publisher_root = true;
    }
    package->built_in = record->built_in;
    package->installed = record->installed;
    package->staged = record->staged;
    package->enabled = record->enabled;
    package->has_private_data = record->has_private_data;
    package->permission_count = record->permission_count;
    package->granted_permission_count = record->granted_permission_count;
    return projection->count < projection->capacity;
}

size_t pxa_esp_host_package_count(void) {
    if (!g_host.initialized) return 0;
    return pxa_esp_package_store_count(PXA_ESP_PACKAGE_VIEW_MANAGED);
}

size_t pxa_esp_host_list_packages(pxa_host_package_info_t *packages,
                                  size_t capacity) {
    pxa_esp_host_public_state_t public_state;
    pxa_esp_host_package_projection_t projection;
    size_t index;
    if (!g_host.initialized || packages == NULL || capacity == 0) return 0;
    projection.packages = packages;
    projection.capacity = capacity;
    projection.count = 0;
    (void)pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_MANAGED,
                                      project_host_package, &projection);
    public_state_snapshot(&public_state);
    for (index = 0; index < projection.count; ++index) {
        packages[index].active =
            public_state.package_active &&
            strcmp(public_state.identity, packages[index].id) == 0;
    }
    return projection.count;
}

static bool copy_package_metadata_text(char *output, size_t capacity,
                                       pxa_bytes_t value) {
    if (output == NULL || capacity == 0 || value.size >= capacity ||
        (value.size != 0 && value.data == NULL)) {
        return false;
    }
    if (value.size != 0) memcpy(output, value.data, value.size);
    output[value.size] = '\0';
    return true;
}

bool pxa_esp_host_resolve_package_metadata(
    const char *identity, const char *locale,
    pxa_host_package_metadata_t *metadata) {
    pxa_package_manifest_t *manifest = NULL;
    pxa_package_metadata_t resolved;
    char root[PXA_ESP_HOST_MAX_PATH];
    uint8_t *encoded = NULL;
    void *workspace = NULL;
    size_t encoded_size;
    size_t workspace_size;
    bool success = false;
    if (!g_host.initialized || identity == NULL || locale == NULL ||
        metadata == NULL) {
        return false;
    }
    memset(metadata, 0, sizeof(*metadata));
    encoded_size =
        pxa_esp_package_store_installed_manifest_size(identity);
    workspace_size =
        pxa_esp_package_store_installed_manifest_workspace_size(identity);
    if (encoded_size == 0 || workspace_size == 0 ||
        (encoded = esp_alloc(encoded_size)) == NULL ||
        (workspace = esp_alloc(workspace_size)) == NULL) {
        goto done;
    }
    if (!pxa_esp_package_store_load_installed(
            identity, root, sizeof(root), workspace, workspace_size, encoded,
            encoded_size, &manifest) ||
        pxa_package_metadata_resolve(
            manifest,
            (pxa_bytes_t){(const uint8_t *)locale, strlen(locale)},
            &resolved) != PXA_STATUS_OK) {
        goto done;
    }
    success = copy_package_metadata_text(
                  metadata->name, sizeof(metadata->name), resolved.name) &&
              copy_package_metadata_text(metadata->description,
                                         sizeof(metadata->description),
                                         resolved.description) &&
              copy_package_metadata_text(metadata->icon_path,
                                         sizeof(metadata->icon_path),
                                         resolved.icon_path);
done:
    free(workspace);
    free(encoded);
    return success;
}

bool pxa_esp_host_deploy_package(const char *identity) {
    return pxa_esp_host_deploy_package_detailed(identity, NULL);
}

bool pxa_esp_host_deploy_package_detailed(
    const char *identity, pxa_host_package_deploy_result_t *result) {
    pxa_esp_package_store_deploy_result_t store_result = {
        .status = PXA_STATUS_BAD_STATE,
        .stage = "host_unavailable",
    };
    if (!g_host.initialized || identity == NULL) {
        store_result.status = identity == NULL ? PXA_STATUS_INVALID_ARGUMENT
                                               : PXA_STATUS_BAD_STATE;
    } else {
        (void)pxa_esp_package_store_deploy_detailed(identity, &store_result);
    }
    if (result != NULL) {
        result->status = store_result.status;
        snprintf(result->stage, sizeof(result->stage), "%s",
                 store_result.stage != NULL ? store_result.stage : "unknown");
    }
    return store_result.status == PXA_STATUS_OK;
}

bool pxa_esp_host_manage_app(pxa_host_app_action_t action,
                             const char *identity) {
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized || identity == NULL) return false;
    public_state_snapshot(&public_state);
    if (public_state.package_active &&
        strcmp(public_state.identity, identity) == 0) {
        return false;
    }
    switch (action) {
        case PXA_HOST_APP_ACTION_INSTALL:
            return pxa_esp_package_store_install(identity);
        case PXA_HOST_APP_ACTION_UNINSTALL:
            return pxa_esp_package_store_uninstall(identity);
        case PXA_HOST_APP_ACTION_CLEAR_DATA:
            return pxa_esp_package_store_clear_data(identity);
        case PXA_HOST_APP_ACTION_ENABLE:
            return pxa_esp_package_store_set_enabled(identity, true);
        case PXA_HOST_APP_ACTION_DISABLE:
            return pxa_esp_package_store_set_enabled(identity, false);
        default:
            return false;
    }
}

typedef struct {
    pxa_host_app_permission_t *permissions;
    size_t capacity;
    size_t count;
} pxa_esp_host_permission_projection_t;

static bool project_public_permission(
    const pxa_esp_package_permission_info_t *permission, void *user_data) {
    pxa_esp_host_permission_projection_t *projection = user_data;
    pxa_host_app_permission_t *target;
    if (permission == NULL || projection == NULL ||
        projection->count >= projection->capacity) {
        return false;
    }
    target = &projection->permissions[projection->count++];
    memset(target, 0, sizeof(*target));
    snprintf(target->name, sizeof(target->name), "%s", permission->name);
    snprintf(target->scope, sizeof(target->scope), "%s", permission->scope);
    target->required = permission->required;
    target->granted = permission->granted;
    return projection->count < projection->capacity;
}

size_t pxa_esp_host_list_app_permissions(
    const char *identity, pxa_host_app_permission_t *permissions,
    size_t capacity) {
    pxa_esp_host_permission_projection_t projection;
    if (!g_host.initialized || identity == NULL || identity[0] == '\0')
        return 0;
    if (permissions == NULL && capacity == 0)
        return pxa_esp_package_store_permission_count(identity);
    if (permissions == NULL || capacity == 0) return 0;
    projection.permissions = permissions;
    projection.capacity = capacity;
    projection.count = 0;
    (void)pxa_esp_package_store_visit_permissions(
        identity, project_public_permission, &projection);
    return projection.count;
}

bool pxa_esp_host_set_permission(const char *identity,
                                 size_t permission_index, bool granted) {
    pxa_esp_host_command_t command;
    pxa_esp_host_public_state_t public_state;
    if (!g_host.initialized || identity == NULL) return false;
    public_state_snapshot(&public_state);
    if (public_state.package_active &&
        strcmp(public_state.identity, identity) == 0) {
        if (permission_index >= public_state.permission_count) return false;
        memset(&command, 0, sizeof(command));
        command.type = (uint8_t)PXA_ESP_HOST_CMD_PERMISSION_SET;
        command.payload.permission_set.permission_index =
            (uint16_t)permission_index;
        command.payload.permission_set.granted = granted ? 1u : 0u;
        snprintf(command.payload.permission_set.identity,
                 sizeof(command.payload.permission_set.identity), "%s",
                 identity);
        return post_command(&command) != 0;
    }
    return pxa_esp_package_store_set_permission(identity, permission_index,
                                                granted);
}

void pxa_esp_host_set_audio_sink(pxa_host_audio_submit_fn submit,
                                 pxa_host_audio_flush_fn flush,
                                 void *context) {
    pxa_esp_audio_set_sink(submit, flush, context);
}

void pxa_esp_host_set_audio_asset_sink(
    pxa_host_audio_asset_play_fn play,
    pxa_host_audio_asset_control_fn control, void *context) {
    pxa_esp_audio_set_asset_sink(play, control, context);
}

#endif /* ESP_PLATFORM */
