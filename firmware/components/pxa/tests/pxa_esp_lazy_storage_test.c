#define _GNU_SOURCE
#include "pxa_test_platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *test_state_root;
#define ESP_PLATFORM 1
#define CONFIG_IDF_TARGET_ESP32S3 1
#define PXSYS_WAMR_ENGINE_ABI "test-wamr-abi"
#define CONFIG_PXA_MOUNT_POINT "/tmp"
#define CONFIG_PXA_STATE_ROOT test_state_root
#include "../src/services/pxa_esp_services.c"

pxa_status_t pxa_esp_assets_begin(const pxa_esp_assets_config_t *config, pxa_assets_backend_t *backend) {
    (void)config; (void)backend; return PXA_STATUS_UNSUPPORTED;
}
pxa_status_t pxa_esp_assets_end(void) { return PXA_STATUS_OK; }
void pxa_esp_assets_io_stats(pxa_esp_asset_io_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
}
void pxa_esp_assets_stats(pxa_asset_cache_stats_t *stats, size_t *metadata, size_t *stack) {
    memset(stats, 0, sizeof(*stats)); *metadata = *stack = 0;
}
void vTaskDelay(TickType_t ticks) { (void)ticks; assert(!"unexpected wait"); }

static void *allocations[32];
static size_t allocation_count;
static int fail_workspace;

void test_log(const char *tag, const char *format, ...) {
    (void)tag;
    (void)format;
}

void test_enter(void) {}
void test_leave(void) {}

void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t count, size_t size, unsigned caps) {
    (void)caps;
    return calloc(count, size);
}

void *heap_caps_realloc(void *memory, size_t size, unsigned caps) {
    (void)caps;
    return realloc(memory, size);
}

void heap_caps_free(void *memory) { free(memory); }

esp_err_t esp_read_mac(uint8_t *address, int type) {
    (void)type;
    memset(address, 0x42, 6);
    return ESP_OK;
}

void pxa_esp_permission_store_bind(pxa_permission_store_t *store) {
    memset(store, 0, sizeof(*store));
}

int pxa_esp_net_backend(pxa_net_backend_t *output,
                        pxa_esp_net_notify_fn notify, void *context) {
    (void)notify;
    (void)context;
    memset(output, 0, sizeof(*output));
    return 1;
}

void pxa_esp_net_reset_requests(void) {}

void pxa_esp_audio_backend(pxa_audio_backend_t *output) {
    memset(output, 0, sizeof(*output));
}

void pxa_esp_audio_reset_sessions(void) {}

void pxa_esp_surface_backend(pxa_surface_backend_t *output) {
    memset(output, 0, sizeof(*output));
}

void pxa_esp_game_render_backend(pxa_game_render_backend_t *output) {
    memset(output, 0, sizeof(*output));
}

void pxa_esp_game_render_get_scale_profile(
    pxa_game_render_target_profile_t *profile) {
    *profile = (pxa_game_render_target_profile_t){
        0, 0, PXA_GAME_RENDER_SCALE_MASK_1X, PXA_GAME_RENDER_SCALE_1X};
}

static void *test_allocate(void *context, size_t size) {
    void *memory;
    (void)context;
    if (fail_workspace) return NULL;
    assert(allocation_count < 32);
    memory = malloc(size);
    assert(memory != NULL);
    allocations[allocation_count++] = memory;
    return memory;
}

static void release_workspaces(void) {
    while (allocation_count) free(allocations[--allocation_count]);
}

int64_t esp_timer_get_time(void) { return 1000; }

static pxa_status_t startup_ui_begin(void *context,
    const pxa_ui_transaction_info_t *info, void **transaction) {
    (void)context; (void)info; (void)transaction;
    assert(!"startup must not submit a UI transaction");
    return PXA_STATUS_INTERNAL;
}
static pxa_status_t startup_ui_apply(void *context, void *transaction,
    const pxa_ui_command_view_t *command, void **created) {
    (void)context; (void)transaction; (void)command; (void)created;
    return PXA_STATUS_INTERNAL;
}
static pxa_status_t startup_ui_commit(void *context, void *transaction) {
    (void)context; (void)transaction; return PXA_STATUS_INTERNAL;
}
static void startup_ui_cancel(void *context, void *transaction) {
    (void)context; (void)transaction;
}
static pxa_status_t startup_ui_canvas(void *context,
    const pxa_ui_canvas_view_t *frame, pxa_ui_release_fn release,
    void *release_context) {
    (void)context; (void)frame; (void)release; (void)release_context;
    return PXA_STATUS_INTERNAL;
}

static void test_display_startup(pxa_runtime_t *runtime) {
    for (unsigned variant = 0; variant < 2; ++variant) {
        pxa_esp_services_t services = {0};
        pxa_esp_services_config_t host = {0};
        pxa_esp_services_result_t result;
        pxa_ui_backend_t backend = {0};
        pxa_ui_environment_t environment;
        pxa_component_t component;
        uint8_t encoded[128]; size_t encoded_size;
        host.runtime = runtime; host.allocate = test_allocate;
        host.primary_width = host.primary_height = 480;
        host.density_q16 = variant ? 124928u : 0; // Mosaico 305 DPI / legacy default.
        host.display_shape = 1;
        for (unsigned i = 0; i < 4; ++i) {
            host.safe_insets[i] = 12; host.corner_radii[i] = 58;
        }
        assert(initialize_window_ui(&services, &host, &result) == PXA_STATUS_OK);
        assert(pxa_component_create(runtime, variant+1, &component) == PXA_STATUS_OK);
        assert(pxa_component_begin_start(runtime, component) == PXA_STATUS_OK);
        backend.struct_size = sizeof(backend);
        backend.begin = startup_ui_begin; backend.apply = startup_ui_apply;
        backend.commit = startup_ui_commit; backend.cancel = startup_ui_cancel;
        backend.present_canvas = startup_ui_canvas;
        assert(pxa_ui_bind(services.ui, component, &backend) == PXA_STATUS_OK);
        assert(pxa_ui_get_environment(services.ui, component,
            PXA_UI_PRIMARY_SURFACE, &environment) == PXA_STATUS_OK);
        assert(environment.width == 480 && environment.height == 480);
        assert(environment.density_q16 == (variant ? 124928u : 65536u));
        assert(environment.font_scale_q16 == 65536u);
        assert(environment.safe_insets[0] == 12 && environment.corner_radii[0] == 58);
        assert(pxa_ui_encode_environment(&environment, encoded, sizeof(encoded),
                                        &encoded_size) == PXA_STATUS_OK);
        assert(pxa_read_u16(encoded+24) == 4 &&
               pxa_read_u32(encoded+28) == environment.density_q16);
        // prepare_start cannot send environment events; onStart needs the
        // correct snapshot without going through the running event mailbox.
        assert(pxa_event_post_message(runtime, component, PXA_UI_SERVICE_ID,
            PXA_UI_ENVIRONMENT_CHANGED, 0, (pxa_bytes_t){encoded, encoded_size},
            1, 0) == PXA_STATUS_BAD_STATE);
        assert(pxa_component_finish_start(runtime, component, PXA_STATUS_OK) == PXA_STATUS_OK);
        pxa_esp_services_destroy(&services); release_workspaces();
    }
    puts("ESP Guest UI startup: default/305 DPI encoded before onStart, no startup events OK");
}

int main(void) {
    char root[] = "/tmp/pxa-lazy-storage-XXXXXX";
    char path[256];
    pxa_esp_services_t services = {0};
    pxa_esp_services_config_t host = {0};
    pxa_esp_services_result_t result;
    pxa_package_manifest_t manifest = {0};
    pxa_runtime_limits_t limits;
    pxa_runtime_t *runtime;
    void *workspace;
    size_t count, size;
    uint8_t bytes[8];
    const pxa_bytes_t key = {(const uint8_t *)"test", 4};
    const pxa_bytes_t value = {(const uint8_t *)"saved", 5};
    assert(mkdtemp(root) != NULL);
    test_state_root = root + strlen("/tmp/");
    /* A fresh install has neither the private data directory nor the app's
     * subdirectory. No file service is declared or initialized in this test. */
    snprintf(path, sizeof(path), "%s/data/app", root);
    assert(access(path, F_OK) != 0);
    pxa_runtime_limits_init(&limits);
    workspace = malloc(pxa_runtime_workspace_size(&limits));
    assert(pxa_runtime_init(workspace, pxa_runtime_workspace_size(&limits),
                            &limits, &runtime) == PXA_STATUS_OK);
    test_display_startup(runtime);
    host.runtime = runtime; host.identity = "app";
    host.allocate = test_allocate; host.manifest = &manifest;
    host.work_epoch = 1;
    assert(initialize_storage(&services, &host, &result) == PXA_STATUS_OK);
    assert(access(path, F_OK) == 0);
    assert(services.posix_storage_workspace == NULL);
    assert(initialize_scheduler(&services, &host, &result) == PXA_STATUS_OK);
    assert(services.posix_storage_workspace == NULL && services.scheduler_store == NULL);
    assert(!pxa_scheduler_has_pending(services.scheduler));
    count = allocation_count;
    fail_workspace = 1;
    assert(lazy_storage_set(&services, key, value) == PXA_STATUS_RESOURCE_LIMIT);
    assert(allocation_count == count && services.posix_storage == NULL);
    fail_workspace = 0;
    assert(lazy_storage_set(&services, key, value) == PXA_STATUS_OK);
    assert(allocation_count == count + 1 && services.posix_storage != NULL);
    assert(lazy_storage_get(&services, key, bytes, sizeof(bytes), &size) == PXA_STATUS_OK);
    assert(size == 5 && memcmp(bytes, "saved", 5) == 0);
    assert(allocation_count == count + 1);
    pxa_esp_services_destroy(&services); release_workspaces();
    assert(initialize_storage(&services, &host, &result) == PXA_STATUS_OK);
    assert(services.posix_storage == NULL);
    assert(!storage_snapshots_absent(&services));
    assert(initialize_scheduler(&services, &host, &result) == PXA_STATUS_OK);
    assert(services.scheduler_store != NULL && services.posix_storage != NULL);
    assert(lazy_storage_get(&services, key, bytes, sizeof(bytes), &size) == PXA_STATUS_OK);
    assert(size == 5 && memcmp(bytes, "saved", 5) == 0);
    /* An upgrade must retain the old canonical-identity directory's KV data
     * and expose the same directory to both file and storage services. */
    pxa_esp_services_destroy(&services); release_workspaces();
    {
        char identity[80], legacy[256], managed[256], resolved[256];
        memset(identity, 'a', 64);
        strcpy(identity + 64, ":app");
        snprintf(legacy, sizeof(legacy), "%s/data/%s", root, identity);
        snprintf(managed, sizeof(managed), "%s", legacy);
        managed[strlen(root) + strlen("/data/") + 64] = '~';
        assert(rename(path, legacy) == 0);
        host.identity = identity;
        assert(initialize_storage(&services, &host, &result) == PXA_STATUS_OK);
        assert(access(legacy, F_OK) != 0 && access(managed, F_OK) == 0);
        assert(lazy_storage_get(&services, key, bytes, sizeof(bytes), &size) == PXA_STATUS_OK);
        assert(size == 5 && memcmp(bytes, "saved", 5) == 0);
        assert(prepare_private_root(resolved, sizeof(resolved), identity));
        assert(strcmp(resolved, managed) == 0);
        /* A conflicting legacy directory is retained, never overwritten or
         * merged into an existing managed application's data. */
        assert(mkdir(legacy, 0700) == 0);
        assert(prepare_private_root(resolved, sizeof(resolved), identity));
        assert(access(legacy, F_OK) == 0 && access(managed, F_OK) == 0);
        assert(lazy_storage_remove(&services, key) == PXA_STATUS_OK);
        pxa_esp_services_destroy(&services); release_workspaces();
    }
    pxa_runtime_deinit(runtime); free(workspace);
    assert(pxa_posix_fs_remove_tree(root) == PXA_STATUS_OK);
    return 0;
}
