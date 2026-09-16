#define _GNU_SOURCE
#include "pxa_test_platform.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *test_state_root;
#define ESP_PLATFORM 1
#define CONFIG_PXA_MOUNT_POINT "/tmp"
#define CONFIG_PXA_STATE_ROOT test_state_root
#include "../src/services/pxa_esp_services.c"

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
    snprintf(path, sizeof(path), "%s/data", root); assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof(path), "%s/data/app", root); assert(mkdir(path, 0700) == 0);
    pxa_runtime_limits_init(&limits);
    workspace = malloc(pxa_runtime_workspace_size(&limits));
    assert(pxa_runtime_init(workspace, pxa_runtime_workspace_size(&limits),
                            &limits, &runtime) == PXA_STATUS_OK);
    host.runtime = runtime; host.identity = "app";
    host.allocate = test_allocate; host.manifest = &manifest;
    host.work_epoch = 1;
    assert(initialize_storage(&services, &host, &result) == PXA_STATUS_OK);
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
    assert(lazy_storage_remove(&services, key) == PXA_STATUS_OK);
    pxa_esp_services_destroy(&services); release_workspaces();
    pxa_runtime_deinit(runtime); free(workspace);
    assert(pxa_posix_fs_remove_tree(root) == PXA_STATUS_OK);
    return 0;
}
