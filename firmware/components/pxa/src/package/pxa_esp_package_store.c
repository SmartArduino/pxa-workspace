/* ESP32 package store on the C stack: catalogs product-owned built-ins and
 * installs verified Inbox Packages (posix adapter, no flock), persisting the
 * disabled policy in NVS. Replaces the legacy C++ package-store implementation. */

#include "pxa_esp_package_store.h"
#include "pxa_package_identity_match.h"
#include "pxa_app_locators.h"
#include "pxa_publisher_profile.h"
#include "pxa_publisher_binding.h"
#include "pxa_locator_migration.h"
#include "pxa_esp_fs_roots.h"
#include "pxa_esp_package_policy.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "pxa/pxa_publisher_namespace.h"

/* LittleFS/FAT mounts on ESP-IDF are link-free; its libc has no lstat.
 * Native adapter tests retain POSIX no-follow checks. */
#if !defined(__linux__) && !defined(__APPLE__)
#define lstat stat
#include "pxa/esp/pxa_esp_posix_shim.h"
#define fstat pxa_esp_fstat
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pxa/esp/pxa_esp_mbedtls.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/posix/pxa_posix_installer.h"
#include "pxa/pxa_publisher_trust.h"
#include "pxa_esp_permission_store.h"

#if defined(__cplusplus)
extern "C" {
#endif
size_t pxa_platform_trusted_namespace_issuers(
    const PxaPublisherNamespaceIssuer **output_keys) __attribute__((weak));
size_t pxa_platform_trusted_publishers(
    const PxaTrustedPublisherKey **output_keys) __attribute__((weak));
#if CONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY
size_t pxa_bundled_development_publishers(
    const PxaTrustedPublisherKey **output_keys);
#endif
#if defined(__cplusplus)
}
#endif

#define PXA_ESP_PACKAGE_TAG "PxaSignedStore"

#ifndef CONFIG_PXA_MOUNT_POINT
#define CONFIG_PXA_MOUNT_POINT "/assets"
#endif
/* sdkconfig.h provides these when PXA is enabled (components/pxa/Kconfig). */
#ifndef CONFIG_PXA_BUILTIN_PACKAGE_ROOT
#define CONFIG_PXA_BUILTIN_PACKAGE_ROOT "system/pxa/builtin"
#endif
#ifndef CONFIG_PXA_STATE_ROOT
#define CONFIG_PXA_STATE_ROOT "pxa-state"
#endif
#ifndef CONFIG_PXA_PACKAGES_DIR
#define CONFIG_PXA_PACKAGES_DIR "packages"
#endif
#ifndef CONFIG_PXA_INBOX_DIR
#define CONFIG_PXA_INBOX_DIR "inbox"
#endif
#ifndef CONFIG_PXA_PRIVATE_DATA_DIR
#define CONFIG_PXA_PRIVATE_DATA_DIR "data"
#endif
#define PXA_ESP_PACKAGE_MAX_ID PXA_ESP_PACKAGE_APP_ID_BYTES
#define PXA_ESP_PACKAGE_MAX_IDENTITY PXA_ESP_PACKAGE_ID_BYTES
#define PXA_ESP_PACKAGE_MAX_PATH PXA_ESP_PACKAGE_ROOT_BYTES
#define PXA_ESP_PACKAGE_MAX_NAME PXA_ESP_PACKAGE_NAME_BYTES
#define PXA_ESP_PACKAGE_MAX_VERSION PXA_ESP_PACKAGE_VERSION_BYTES
#define PXA_ESP_PACKAGE_MAX_ICON_PATH PXA_ESP_PACKAGE_ICON_PATH_BYTES
#define PXA_ESP_PACKAGE_MAX_ASSET_PATH \
    (PXA_ESP_PACKAGE_MAX_PATH + PXA_ESP_PACKAGE_MAX_ICON_PATH)
#define PXA_ESP_PACKAGE_ENTRY_GROWTH 4u
#define PXA_ESP_PACKAGE_SYNC_TASK_STACK_SIZE 10240
#define PXA_ESP_PACKAGE_SYNC_TASK_PRIORITY 1

typedef struct {
    char id[PXA_ESP_PACKAGE_MAX_ID];
    char identity[PXA_ESP_PACKAGE_MAX_IDENTITY];
    char storage_key[PXA_ESP_PACKAGE_MAX_IDENTITY];
    char name[PXA_ESP_PACKAGE_MAX_NAME];
    char version[PXA_ESP_PACKAGE_MAX_VERSION];
    char staged_version[PXA_ESP_PACKAGE_MAX_VERSION];
    char icon_path[PXA_ESP_PACKAGE_MAX_ICON_PATH];
    char staged_icon_path[PXA_ESP_PACKAGE_MAX_ICON_PATH];
    char root[PXA_ESP_PACKAGE_MAX_PATH];
    char inbox_root[PXA_ESP_PACKAGE_MAX_PATH];
    uint8_t publisher_key_id[PXA_PACKAGE_DIGEST_BYTES];
    uint64_t release_sequence;
    uint64_t staged_release_sequence;
    uint32_t builtin_scan_generation;
    uint16_t lineage_count;
    uint16_t staged_lineage_count;
    uint8_t has_publisher_key_id;
    uint8_t has_release_sequence;
    uint8_t has_staged_release_sequence;
    uint8_t permission_count;
    uint8_t granted_permission_count;
    uint8_t installed;
    uint8_t staged;
    uint8_t staged_assets_available;
    uint8_t built_in;
    uint8_t enabled;
    uint8_t has_private_data;
    uint8_t input_method;
} pxa_esp_package_entry_t;

typedef struct {
    uint8_t initialized;
    pxa_app_locators_t locators;
    pxa_posix_installer_t *installer;
    void *installer_workspace;
    pxa_posix_installer_config_t installer_config;
    void *manifest_workspace;
    size_t manifest_workspace_size;
    uint8_t *encoded;
    size_t encoded_capacity;
    pxa_esp_mbedtls_trust_t trust;
    pxa_esp_mbedtls_publisher_key_t *trust_keys;
    size_t trust_key_count;
    size_t trust_key_capacity;
    pxa_package_limits_t limits;
    pxa_esp_package_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    char state_root[PXA_ESP_PACKAGE_MAX_PATH];
    char packages_root[PXA_ESP_PACKAGE_MAX_PATH];
    char data_root[PXA_ESP_PACKAGE_MAX_PATH];
    char builtin_root[PXA_ESP_PACKAGE_MAX_PATH];
    char inbox_root[PXA_ESP_PACKAGE_MAX_PATH];
    SemaphoreHandle_t metadata_lock;
    SemaphoreHandle_t transaction_lock;
    SemaphoreHandle_t sync_lock;
    TaskHandle_t sync_task;
    pxa_esp_package_store_sync_fn sync_callback;
    void *sync_callback_user_data;
    uint32_t builtin_scan_generation;
} pxa_esp_package_store_t;

static pxa_esp_package_store_t *g_store;

static int manifest_workspace_for_encoded(const uint8_t *encoded,
                                          size_t encoded_size,
                                          pxa_package_limits_t *limits_out,
                                          size_t *workspace_size_out);

static bool store_is_initialized(void) {
    return g_store != NULL && g_store->initialized;
}

static bool allocate_store(void) {
    if (g_store != NULL) return true;

    g_store = heap_caps_calloc(1, sizeof(*g_store),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_store == NULL) {
        g_store = heap_caps_calloc(1, sizeof(*g_store),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (g_store == NULL) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG,
                 "Package store allocation failed: size=%u",
                 (unsigned)sizeof(*g_store));
        return false;
    }
    return true;
}

static int take_lock(SemaphoreHandle_t lock) {
    /* In-flight cooperative IME verification owns the transaction lease.
     * Refuse accidental owner re-entry instead of deadlocking on its mutex. */
    return lock != NULL && xSemaphoreGetMutexHolder(lock) != xTaskGetCurrentTaskHandle() &&
        xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE;
}

static void release_lock(SemaphoreHandle_t lock) {
    if (lock != NULL) (void)xSemaphoreGive(lock);
}

static void *esp_alloc(size_t size) {
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return memory;
}

static int ensure_encoded_capacity(size_t required) {
    uint8_t *resized;
    if (required == 0) return 0;
    if (g_store->encoded_capacity >= required) return 1;
    if (g_store->encoded == NULL) {
        resized = esp_alloc(required);
    } else {
        resized = heap_caps_realloc(g_store->encoded, required,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resized == NULL) {
            resized = heap_caps_realloc(g_store->encoded, required,
                                        MALLOC_CAP_8BIT);
        }
    }
    if (resized == NULL) return 0;
    g_store->encoded = resized;
    g_store->encoded_capacity = required;
    return 1;
}

static int ensure_manifest_workspace_capacity(size_t required) {
    void *resized;
    if (required == 0) return 0;
    if (g_store->manifest_workspace_size >= required) return 1;
    if (g_store->manifest_workspace == NULL) {
        resized = esp_alloc(required);
    } else {
        resized = heap_caps_realloc(g_store->manifest_workspace, required,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resized == NULL) {
            resized = heap_caps_realloc(g_store->manifest_workspace, required,
                                        MALLOC_CAP_8BIT);
        }
    }
    if (resized == NULL) return 0;
    g_store->manifest_workspace = resized;
    g_store->manifest_workspace_size = required;
    return 1;
}

static int bind_store_result_buffers(pxa_posix_installer_result_t *result) {
    if (result == NULL || g_store == NULL || g_store->encoded == NULL ||
        g_store->encoded_capacity == 0 ||
        g_store->manifest_workspace == NULL ||
        g_store->manifest_workspace_size == 0) {
        return 0;
    }
    result->manifest_workspace = g_store->manifest_workspace;
    result->manifest_workspace_size = g_store->manifest_workspace_size;
    result->encoded = g_store->encoded;
    result->encoded_capacity = g_store->encoded_capacity;
    return 1;
}

static pxa_status_t read_source_into_store(
    const char *source, pxa_posix_installer_result_t *result, int manifest_only) {
    pxa_package_limits_t actual_limits;
    size_t manifest_size;
    size_t manifest_workspace_size;
    pxa_status_t status;
    if (result == NULL) return PXA_STATUS_INVALID_ARGUMENT;
    status = pxa_posix_installer_source_manifest_size(
        g_store->installer, source, &manifest_size);
    if (status != PXA_STATUS_OK) return status;
    if (!ensure_encoded_capacity(manifest_size) ||
        !ensure_manifest_workspace_capacity(1)) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (!bind_store_result_buffers(result)) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    status = (manifest_only ? pxa_posix_installer_read_signed_manifest :
                              pxa_posix_installer_verify_source)(g_store->installer, source,
                                               result);
    if (status != PXA_STATUS_RESOURCE_LIMIT ||
        !manifest_workspace_for_encoded(g_store->encoded, manifest_size,
                                        &actual_limits,
                                        &manifest_workspace_size) ||
        !ensure_manifest_workspace_capacity(manifest_workspace_size)) {
        return status;
    }
    if (!bind_store_result_buffers(result)) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    return (manifest_only ? pxa_posix_installer_read_signed_manifest :
                              pxa_posix_installer_verify_source)(g_store->installer, source,
                                             result);
}

static pxa_status_t verify_source_into_store(
    const char *source, pxa_posix_installer_result_t *result) {
    return read_source_into_store(source, result, 0);
}

static void release_store(void) {
    if (g_store == NULL) return;
    pxa_esp_package_policy_deinitialize();
    if (g_store->metadata_lock != NULL)
        vSemaphoreDelete(g_store->metadata_lock);
    if (g_store->transaction_lock != NULL)
        vSemaphoreDelete(g_store->transaction_lock);
    if (g_store->sync_lock != NULL) vSemaphoreDelete(g_store->sync_lock);
    if (g_store->installer != NULL)
        pxa_posix_installer_deinit(g_store->installer);
    heap_caps_free(g_store->entries);
    heap_caps_free(g_store->trust_keys);
    heap_caps_free(g_store->manifest_workspace);
    heap_caps_free(g_store->encoded);
    heap_caps_free(g_store->installer_workspace);
    heap_caps_free(g_store);
    g_store = NULL;
}

static int is_directory(const char *path) {
    struct stat metadata;
    return stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode);
}

static int is_regular_file(const char *path) {
    struct stat metadata;
    return stat(path, &metadata) == 0 && S_ISREG(metadata.st_mode);
}

static int has_suffix(const char *value, const char *suffix) {
    size_t value_size;
    size_t suffix_size;
    if (value == NULL || suffix == NULL) return 0;
    value_size = strlen(value);
    suffix_size = strlen(suffix);
    return suffix_size <= value_size &&
           memcmp(value + value_size - suffix_size, suffix, suffix_size) == 0;
}

/* Staging contains both directory packages and single-file containers. The
 * POSIX filesystem remove_tree API accepts directories only. */
static pxa_status_t remove_staging_source(const char *source) {
    struct stat metadata;
    /* LittleFS has no symlinks. */
    if (stat(source, &metadata) != 0)
        return errno == ENOENT ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
    if (S_ISDIR(metadata.st_mode)) return pxa_posix_fs_remove_tree(source);
    return unlink(source) == 0 || errno == ENOENT
        ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
}

static int ensure_directory_tree(const char *relative) {
    char current[PXA_ESP_PACKAGE_MAX_PATH];
    char component[64];
    size_t begin = 0;
    snprintf(current, sizeof(current), "%s", CONFIG_PXA_MOUNT_POINT);
    while (begin < strlen(relative)) {
        size_t separator;
        size_t end;
        struct stat metadata;
        char *slash = strchr(relative + begin, '/');
        separator = slash != NULL
                        ? (size_t)(slash - relative)
                        : strlen(relative);
        end = separator;
        snprintf(component, sizeof(component), "%.*s",
                 (int)(end - begin), relative + begin);
        snprintf(current + strlen(current),
                 sizeof(current) - strlen(current), "/%s", component);
        if (stat(current, &metadata) != 0) {
            if (errno != ENOENT || mkdir(current, 0700) != 0) return 0;
        } else if (!S_ISDIR(metadata.st_mode)) {
            return 0;
        }
        if (slash == NULL) break;
        begin = separator + 1;
    }
    return 1;
}

static int safe_app_id(const char *value) {
    size_t size;
    size_t index;
    if (value == NULL) return 0;
    size = strlen(value);
    if (size == 0 || size > 64) return 0;
    for (index = 0; index < size; ++index) {
        const char byte = value[index];
        const int allowed = (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '.' ||
                            byte == '_' || byte == '-';
        if (!allowed) return 0;
    }
    return 1;
}

static int canonical_identity(char *output, size_t capacity,
                              const uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES],
                              const char *app_id) {
    static const char digits[] = "0123456789abcdef";
    size_t offset = 0;
    size_t index;
    size_t app_id_size;
    if (output == NULL || publisher_root == NULL || !safe_app_id(app_id)) return 0;
    app_id_size = strlen(app_id);
    if (PXA_PACKAGE_DIGEST_BYTES * 2u + 1u + app_id_size + 1u > capacity)
        return 0;
    for (index = 0; index < PXA_PACKAGE_DIGEST_BYTES; ++index) {
        output[offset++] = digits[publisher_root[index] >> 4];
        output[offset++] = digits[publisher_root[index] & 0x0fu];
    }
    output[offset++] = ':';
    memcpy(output + offset, app_id, app_id_size + 1u);
    return 1;
}

static int filesystem_identity(
    char *output, size_t capacity,
    const uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES],
    const char *app_id) {
    if (!canonical_identity(output, capacity, publisher_root, app_id)) return 0;
    output[PXA_PACKAGE_DIGEST_BYTES * 2u] = '~';
    return 1;
}

static pxa_esp_package_entry_t *find_entry(const char *identity) {
    size_t index;
    pxa_esp_package_entry_t *legacy = NULL;
    if (identity == NULL) return NULL;
    for (index = 0; index < g_store->entry_count; ++index) {
        if (g_store->entries[index].identity[0] != '\0' &&
            strcmp(g_store->entries[index].identity, identity) == 0) {
            return &g_store->entries[index];
        }
        if (strcmp(g_store->entries[index].id, identity) == 0) {
            if (legacy != NULL) return NULL;
            legacy = &g_store->entries[index];
        }
    }
    return legacy;
}

static size_t manifest_size_at_root(const char *root) {
    struct stat metadata;
    char path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    int path_size;
    if (root == NULL || root[0] == '\0') return 0;
    path_size = snprintf(path, sizeof(path), "%s/manifest.pxm", root);
    if (path_size < 0 || (size_t)path_size >= sizeof(path) ||
        stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size <= 0 || (uint64_t)metadata.st_size > SIZE_MAX) {
        return 0;
    }
    return (size_t)metadata.st_size;
}

static int read_manifest_file(const char *path, uint8_t *encoded,
                              size_t encoded_capacity,
                              size_t *encoded_size_out) {
    FILE *file;
    long file_size;
    if (path == NULL || encoded == NULL || encoded_size_out == NULL) {
        return 0;
    }
    *encoded_size_out = 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    file_size = ftell(file);
    if (file_size <= 0 || (uint64_t)file_size > SIZE_MAX ||
        (size_t)file_size > encoded_capacity) {
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    if (fread(encoded, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        return 0;
    }
    fclose(file);
    *encoded_size_out = (size_t)file_size;
    return 1;
}

static int manifest_workspace_for_encoded(const uint8_t *encoded,
                                          size_t encoded_size,
                                          pxa_package_limits_t *limits_out,
                                          size_t *workspace_size_out) {
    pxa_status_t status;
    size_t workspace_size;
    if (encoded == NULL || limits_out == NULL || workspace_size_out == NULL) {
        return 0;
    }
    status = pxa_package_manifest_measure(
        (pxa_bytes_t){encoded, encoded_size}, &g_store->limits, limits_out);
    if (status != PXA_STATUS_OK) return 0;
    workspace_size = pxa_package_manifest_workspace_size(limits_out);
    if (workspace_size == 0) return 0;
    *workspace_size_out = workspace_size;
    return 1;
}

static int load_manifest_file(const char *path, void *manifest_workspace,
                              size_t manifest_workspace_size,
                              uint8_t *encoded, size_t encoded_capacity,
                              pxa_package_manifest_t **manifest_out) {
    pxa_package_limits_t actual_limits;
    size_t encoded_size;
    size_t required_workspace_size;
    pxa_status_t status;
    if (manifest_workspace == NULL || manifest_out == NULL ||
        !read_manifest_file(path, encoded, encoded_capacity, &encoded_size) ||
        !manifest_workspace_for_encoded(encoded, encoded_size, &actual_limits,
                                        &required_workspace_size) ||
        manifest_workspace_size < required_workspace_size) {
        return 0;
    }
    *manifest_out = NULL;
    status = pxa_package_manifest_parse(
        manifest_workspace, manifest_workspace_size,
        (pxa_bytes_t){encoded, encoded_size}, &actual_limits, manifest_out);
    return status == PXA_STATUS_OK && *manifest_out != NULL;
}

static int parse_manifest_file(const char *path,
                               pxa_package_manifest_t **manifest_out) {
    struct stat metadata;
    pxa_package_limits_t actual_limits;
    size_t encoded_size;
    size_t manifest_workspace_size;
    pxa_status_t status;
    if (g_store == NULL || path == NULL || stat(path, &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
        (uint64_t)metadata.st_size > SIZE_MAX ||
        !ensure_encoded_capacity((size_t)metadata.st_size)) {
        return 0;
    }
    if (!read_manifest_file(path, g_store->encoded, g_store->encoded_capacity,
                            &encoded_size) ||
        !manifest_workspace_for_encoded(g_store->encoded, encoded_size,
                                        &actual_limits,
                                        &manifest_workspace_size) ||
        !ensure_manifest_workspace_capacity(manifest_workspace_size)) {
        return 0;
    }
    *manifest_out = NULL;
    status = pxa_package_manifest_parse(
        g_store->manifest_workspace, g_store->manifest_workspace_size,
        (pxa_bytes_t){g_store->encoded, encoded_size}, &actual_limits,
        manifest_out);
    return status == PXA_STATUS_OK && *manifest_out != NULL;
}

static int parse_entry_manifest(const pxa_esp_package_entry_t *entry,
                                pxa_package_manifest_t **manifest_out) {
    char manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    int path_size;
    if (entry == NULL || !entry->installed || manifest_out == NULL) return 0;
    path_size = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.pxm",
                         entry->root);
    if (path_size < 0 || (size_t)path_size >= sizeof(manifest_path)) return 0;
    return parse_manifest_file(manifest_path, manifest_out);
}

static void copy_bytes_text(char *output, size_t capacity, pxa_bytes_t value) {
    size_t size;
    if (output == NULL || capacity == 0) return;
    size = value.size < capacity - 1 ? value.size : capacity - 1;
    if (size != 0 && value.data != NULL) memcpy(output, value.data, size);
    output[size] = '\0';
}

static void copy_scope_text(char *output, size_t capacity, pxa_bytes_t scope) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    size_t offset = 0;
    int printable = 1;
    if (output == NULL || capacity == 0) return;
    if (scope.size == 0) {
        snprintf(output, capacity, "-");
        return;
    }
    for (index = 0; index < scope.size; ++index) {
        if (scope.data[index] < 0x20 || scope.data[index] > 0x7e) {
            printable = 0;
            break;
        }
    }
    if (printable) {
        copy_bytes_text(output, capacity, scope);
        return;
    }
    if (capacity > 1) output[offset++] = '0';
    if (capacity > 2) output[offset++] = 'x';
    for (index = 0; index < scope.size && offset + 2 < capacity; ++index) {
        output[offset++] = digits[scope.data[index] >> 4];
        output[offset++] = digits[scope.data[index] & 0x0f];
    }
    output[offset] = '\0';
}

static int permission_granted(const char *identity,
                              const pxa_package_permission_t *permission) {
    pxa_permission_decision_t decision = PXA_PERMISSION_DENY;
    if (identity == NULL || permission == NULL) return 0;
    (void)pxa_esp_permission_store_load(
        NULL, (pxa_bytes_t){(const uint8_t *)identity, strlen(identity)},
        permission->name, permission->scope, &decision);
    return decision == PXA_PERMISSION_ALLOW;
}

static int manifest_matches_app_id(const pxa_package_manifest_t *manifest,
                                   const char *app_id) {
    const size_t app_id_size = app_id == NULL ? 0 : strlen(app_id);
    return app_id != NULL && manifest != NULL && manifest->app_id.data != NULL &&
           manifest->app_id.size == app_id_size &&
           memcmp(manifest->app_id.data, app_id, app_id_size) == 0;
}

static uint16_t manifest_lineage_count(
    const pxa_package_manifest_t *manifest) {
    pxa_package_publisher_lineage_t lineage;
    if (manifest == NULL ||
        pxa_package_publisher_lineage_parse(manifest, &lineage) !=
            PXA_STATUS_OK) {
        return 0;
    }
    return lineage.link_count;
}

static int manifest_publisher_root(
    const pxa_package_manifest_t *manifest,
    uint8_t output[PXA_PACKAGE_DIGEST_BYTES]) {
    pxa_package_publisher_lineage_t lineage;
    if (manifest == NULL || output == NULL ||
        pxa_package_publisher_lineage_parse(manifest, &lineage) !=
            PXA_STATUS_OK ||
        lineage.root_key_id == NULL) {
        return 0;
    }
    memcpy(output, lineage.root_key_id, PXA_PACKAGE_DIGEST_BYTES);
    return 1;
}

static void cache_manifest_path(char *output, size_t capacity,
                                pxa_bytes_t path) {
    if (output == NULL || capacity == 0) return;
    output[0] = '\0';
    if (path.data == NULL || path.size == 0 || path.size >= capacity) return;
    memcpy(output, path.data, path.size);
    output[path.size] = '\0';
}

static uint8_t granted_permission_count(
    const char *identity, const pxa_package_manifest_t *manifest) {
    size_t index;
    size_t granted = 0;
    if (identity == NULL || manifest == NULL) return 0;
    for (index = 0; index < manifest->permission_count; ++index) {
        if (permission_granted(identity, &manifest->permissions[index])) {
            ++granted;
        }
    }
    return granted > UINT8_MAX ? UINT8_MAX : (uint8_t)granted;
}

static void update_entry_metadata(pxa_esp_package_entry_t *entry,
                                  const pxa_package_manifest_t *manifest) {
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    if (entry == NULL || manifest == NULL) return;
    if (manifest->name.size != 0 && manifest->name.size < sizeof(entry->name)) {
        memcpy(entry->name, manifest->name.data, manifest->name.size);
        entry->name[manifest->name.size] = '\0';
    } else {
        snprintf(entry->name, sizeof(entry->name), "%s", entry->id);
    }
    if (manifest->version.size != 0 &&
        manifest->version.size < sizeof(entry->version)) {
        memcpy(entry->version, manifest->version.data,
               manifest->version.size);
        entry->version[manifest->version.size] = '\0';
    } else {
        snprintf(entry->version, sizeof(entry->version), "0.0");
    }
    if (manifest_publisher_root(manifest, publisher_root)) {
        memcpy(entry->publisher_key_id, publisher_root, sizeof(publisher_root));
        entry->has_publisher_key_id = 1;
        (void)canonical_identity(entry->identity, sizeof(entry->identity),
                                 publisher_root, entry->id);
        (void)filesystem_identity(entry->storage_key, sizeof(entry->storage_key),
                                  publisher_root, entry->id);
        /* Catalog refreshes run again after reboot and after an update. Never
         * replace an already committed private locator with its legacy key. */
        char relative[130]; uint16_t flags = 0;
        if (pxa_app_locators_find(&g_store->locators, publisher_root, entry->id,
                &flags, relative, sizeof(relative)) == PXA_STATUS_OK &&
            !(flags & PXA_LOCATOR_DATA_LEGACY)) {
            snprintf(entry->storage_key, sizeof(entry->storage_key), "%s", relative);
        }
    }
}

static void update_installed_metadata(
    pxa_esp_package_entry_t *entry,
    const pxa_package_manifest_t *manifest) {
    if (entry == NULL || manifest == NULL) return;
    entry->input_method = 0;
    for (uint16_t i = 0; i < manifest->component_count; ++i)
        if (pxa_ime_is_provider(&manifest->components[i])) entry->input_method = 1;
    update_entry_metadata(entry, manifest);
    cache_manifest_path(entry->icon_path, sizeof(entry->icon_path),
                        manifest->icon_path);
    entry->has_release_sequence = manifest->has_release_sequence != 0;
    entry->release_sequence = manifest->release_sequence;
    entry->lineage_count = manifest_lineage_count(manifest);
    entry->permission_count =
        manifest->permission_count > UINT8_MAX
            ? UINT8_MAX
            : (uint8_t)manifest->permission_count;
    entry->granted_permission_count =
        granted_permission_count(entry->identity, manifest);
}

static void update_staged_metadata(pxa_esp_package_entry_t *entry,
                                   const pxa_package_manifest_t *manifest) {
    if (entry == NULL || manifest == NULL) return;
    copy_bytes_text(entry->staged_version, sizeof(entry->staged_version),
                    manifest->version);
    cache_manifest_path(entry->staged_icon_path,
                        sizeof(entry->staged_icon_path), manifest->icon_path);
    entry->has_staged_release_sequence =
        manifest->has_release_sequence != 0;
    entry->staged_release_sequence = manifest->release_sequence;
    entry->staged_lineage_count = manifest_lineage_count(manifest);
}

static pxa_esp_package_entry_t *reserve_entry(
    const char *app_id,
    const uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES]) {
    pxa_esp_package_entry_t *entry;
    pxa_esp_package_entry_t *resized;
    size_t capacity;
    size_t bytes;
    if (!safe_app_id(app_id)) return NULL;
    char identity[PXA_ESP_PACKAGE_MAX_IDENTITY];
    if (publisher_root == NULL ||
        !canonical_identity(identity, sizeof(identity), publisher_root, app_id)) {
        return NULL;
    }
    entry = find_entry(identity);
    if (entry != NULL) return entry;
    if (g_store->entry_count == g_store->entry_capacity) {
        if (g_store->entry_capacity > SIZE_MAX - PXA_ESP_PACKAGE_ENTRY_GROWTH) {
            return NULL;
        }
        capacity = g_store->entry_capacity + PXA_ESP_PACKAGE_ENTRY_GROWTH;
        if (capacity > SIZE_MAX / sizeof(*g_store->entries)) return NULL;
        bytes = capacity * sizeof(*g_store->entries);
        if (g_store->entries == NULL) {
            resized = esp_alloc(bytes);
        } else {
            resized = heap_caps_realloc(
                g_store->entries, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (resized == NULL) {
                resized = heap_caps_realloc(g_store->entries, bytes,
                                            MALLOC_CAP_8BIT);
            }
        }
        if (resized == NULL) return NULL;
        g_store->entries = resized;
        g_store->entry_capacity = capacity;
    }
    entry = &g_store->entries[g_store->entry_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", app_id);
    snprintf(entry->identity, sizeof(entry->identity), "%s", identity);
    (void)filesystem_identity(entry->storage_key, sizeof(entry->storage_key),
                              publisher_root, app_id);
    memcpy(entry->publisher_key_id, publisher_root, PXA_PACKAGE_DIGEST_BYTES);
    entry->has_publisher_key_id = 1;
    return entry;
}

static int refresh_entry_from_manifest(const char *app_id, const char *root,
                                       int built_in,
                                       const pxa_package_manifest_t *manifest) {
    pxa_esp_package_entry_t *entry;
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    if (!safe_app_id(app_id) || root == NULL ||
        strlen(root) >= PXA_ESP_PACKAGE_MAX_PATH ||
        !manifest_matches_app_id(manifest, app_id) ||
        !manifest_publisher_root(manifest, publisher_root)) {
        return 0;
    }
    entry = reserve_entry(app_id, publisher_root);
    if (entry == NULL) return 0;
    { char relative[130];uint16_t flags;
      pxa_status_t status=pxa_app_locators_find(&g_store->locators,publisher_root,app_id,&flags,relative,sizeof(relative));
      if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return 0;
      if(status==PXA_STATUS_OK && !(flags&PXA_LOCATOR_DATA_LEGACY))
          snprintf(entry->storage_key,sizeof(entry->storage_key),"%s",relative);
    }
    update_installed_metadata(entry, manifest);
    snprintf(entry->root, sizeof(entry->root), "%s", root);
    entry->installed = 1;
    entry->built_in = built_in != 0;
    entry->enabled = !pxa_esp_package_policy_is_disabled(entry->identity);
    {
        char data_path[PXA_ESP_PACKAGE_MAX_PATH];
        int path_size = snprintf(data_path, sizeof(data_path), "%s/%s",
                                 g_store->data_root, entry->storage_key);
        entry->has_private_data =
            path_size >= 0 && (size_t)path_size < sizeof(data_path) &&
            is_directory(data_path);
    }
    return 1;
}

static int refresh_entry_at_root(const char *app_id, const char *root,
                                 int built_in) {
    pxa_package_manifest_t *manifest = NULL;
    char manifest_path[PXA_ESP_PACKAGE_MAX_PATH];
    int path_size;
    if (!safe_app_id(app_id) || root == NULL) return 0;
    path_size = snprintf(manifest_path, sizeof(manifest_path),
                         "%s/manifest.pxm", root);
    if (path_size < 0 || (size_t)path_size >= sizeof(manifest_path) ||
        !parse_manifest_file(manifest_path, &manifest)) {
        return 0;
    }
    return refresh_entry_from_manifest(app_id, root, built_in, manifest);
}

static int refresh_existing_entry(const char *identity) {
    pxa_esp_package_entry_t *entry = find_entry(identity);
    char root[PXA_ESP_PACKAGE_MAX_PATH];
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    int built_in;
    if (entry == NULL || !entry->installed) return 0;
    snprintf(root, sizeof(root), "%s", entry->root);
    snprintf(app_id, sizeof(app_id), "%s", entry->id);
    built_in = entry->built_in != 0;
    return refresh_entry_at_root(app_id, root, built_in);
}

static int load_builtin_candidate(const char *directory_name,
                                  pxa_esp_package_entry_t *candidate) {
    char source[PXA_ESP_PACKAGE_MAX_PATH];
    char manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    pxa_package_manifest_t *manifest = NULL;
    pxa_package_limits_t actual_limits;
    uint8_t *encoded = NULL;
    void *manifest_workspace = NULL;
    size_t encoded_size;
    size_t manifest_workspace_size;
    size_t directory_name_size;
    int success = 0;
    int source_size;
    if (!safe_app_id(directory_name) || candidate == NULL) {
        return 0;
    }
    directory_name_size = strlen(directory_name);
    if (directory_name_size >= sizeof(candidate->id)) return 0;
    source_size = snprintf(source, sizeof(source), "%s/%s",
                           g_store->builtin_root, directory_name);
    if (source_size < 0 || (size_t)source_size >= sizeof(source)) return 0;
    encoded_size = manifest_size_at_root(source);
    if (encoded_size == 0 || (encoded = esp_alloc(encoded_size)) == NULL) {
        goto done;
    }
    source_size = snprintf(manifest_path, sizeof(manifest_path),
                           "%s/manifest.pxm", source);
    if (source_size < 0 || (size_t)source_size >= sizeof(manifest_path) ||
        !read_manifest_file(manifest_path, encoded, encoded_size,
                            &encoded_size) ||
        !manifest_workspace_for_encoded(encoded, encoded_size, &actual_limits,
                                        &manifest_workspace_size) ||
        (manifest_workspace = esp_alloc(manifest_workspace_size)) == NULL ||
        pxa_package_manifest_parse(
            manifest_workspace, manifest_workspace_size,
            (pxa_bytes_t){encoded, encoded_size}, &actual_limits,
            &manifest) != PXA_STATUS_OK ||
        !manifest_matches_app_id(manifest, directory_name)) {
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                 "Reject built-in Package %s: manifest parse or identity "
                 "validation failed", directory_name);
        goto done;
    }
    memset(candidate, 0, sizeof(*candidate));
    memcpy(candidate->id, directory_name, directory_name_size + 1u);
    update_installed_metadata(candidate, manifest);
    snprintf(candidate->root, sizeof(candidate->root), "%s", source);
    candidate->installed = 1;
    candidate->built_in = 1;
    {
        char data_path[PXA_ESP_PACKAGE_MAX_PATH];
        int path_size = snprintf(data_path, sizeof(data_path), "%s/%s",
                                 g_store->data_root,
                                 candidate->storage_key);
        candidate->has_private_data =
            path_size >= 0 && (size_t)path_size < sizeof(data_path) &&
            is_directory(data_path);
    }
    success = 1;
done:
    heap_caps_free(manifest_workspace);
    heap_caps_free(encoded);
    return success;
}

static int stage_inbox_entry(const char *source, pxa_package_manifest_t *verified,
                              pxa_status_t *validation) {
    pxa_package_manifest_t *manifest = verified;
    pxa_esp_package_entry_t *entry;
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    char verified_root[PXA_ESP_PACKAGE_MAX_PATH];
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    pxa_posix_installer_result_t result;
    int source_is_file;
    if (source == NULL) return 0;
    source_is_file = is_regular_file(source);
    if (validation != NULL) *validation = PXA_STATUS_OK;
    if (manifest == NULL) {
        memset(&result, 0, sizeof(result));
        result.struct_size = sizeof(result);
        result.manifest = &manifest;
        result.root = verified_root;
        result.root_capacity = sizeof(verified_root);
        pxa_status_t status = verify_source_into_store(source, &result);
        if (validation != NULL) *validation = status;
        if (status != PXA_STATUS_OK) return 0;
    }
    if (manifest == NULL || manifest->app_id.size == 0 ||
        manifest->app_id.size >= sizeof(app_id)) {
        return 0;
    }
    memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
    app_id[manifest->app_id.size] = '\0';
    if (!safe_app_id(app_id) ||
        !manifest_publisher_root(manifest, publisher_root)) return 0;

    /* Entries are keyed by publisher root plus App ID. Reuse an installed
     * built-in entry with the same identity so PXADB can stage an override;
     * a different publisher naturally receives a distinct entry. */
    entry = reserve_entry(app_id, publisher_root);
    if (entry == NULL ||
        (entry->staged && strcmp(entry->inbox_root, source) != 0)) {
        return 0;
    }
    if (!entry->installed) update_entry_metadata(entry, manifest);
    update_staged_metadata(entry, manifest);
    snprintf(entry->inbox_root, sizeof(entry->inbox_root), "%s", source);
    entry->staged = 1;
    entry->staged_assets_available = !source_is_file;
    return 1;
}

static void remove_entry_at(size_t index) {
    if (index >= g_store->entry_count) return;
    memmove(&g_store->entries[index], &g_store->entries[index + 1],
            (g_store->entry_count - index - 1u) * sizeof(g_store->entries[0]));
    --g_store->entry_count;
}

static int scan_inbox(int cleanup_incomplete) {
    DIR *directory;
    struct dirent *source_entry;
    size_t index;

    directory = opendir(g_store->inbox_root);
    if (directory == NULL && errno != ENOENT) {
        ESP_LOGW(PXA_ESP_PACKAGE_TAG, "Open Package Inbox failed: errno=%d",
                 errno);
        return 0;
    }

    for (index = 0; index < g_store->entry_count; ++index) {
        g_store->entries[index].staged = 0;
        g_store->entries[index].staged_assets_available = 0;
        g_store->entries[index].inbox_root[0] = '\0';
        g_store->entries[index].staged_version[0] = '\0';
        g_store->entries[index].staged_icon_path[0] = '\0';
        g_store->entries[index].has_staged_release_sequence = 0;
        g_store->entries[index].staged_release_sequence = 0;
        g_store->entries[index].staged_lineage_count = 0;
    }

    if (directory != NULL) {
        while ((source_entry = readdir(directory)) != NULL) {
            char source[PXA_ESP_PACKAGE_MAX_PATH];
            int path_size;
            if (strcmp(source_entry->d_name, ".") == 0 ||
                strcmp(source_entry->d_name, "..") == 0 ||
                strncmp(source_entry->d_name, PXA_POSIX_INSTALLER_RECEIVING_PREFIX,
                        sizeof(PXA_POSIX_INSTALLER_RECEIVING_PREFIX) - 1) == 0 ||
                (!cleanup_incomplete && has_suffix(source_entry->d_name, ".pxadb-part"))) {
                continue;
            }
            path_size = snprintf(source, sizeof(source), "%s/%s",
                                 g_store->inbox_root, source_entry->d_name);
            if (path_size < 0 || (size_t)path_size >= sizeof(source) ||
                (!is_directory(source) && !is_regular_file(source))) {
                continue;
            }
            if (cleanup_incomplete && has_suffix(source_entry->d_name, ".pxadb-part")) {
                if (remove_staging_source(source) == PXA_STATUS_OK) {
                    closedir(directory);
                    directory = opendir(g_store->inbox_root);
                    if (directory == NULL) return 0;
                }
                continue;
            }
            pxa_status_t validation = PXA_STATUS_OK;
            if (!stage_inbox_entry(source, NULL, &validation)) {
                ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                         "Ignoring invalid or duplicate Inbox Package: %s",
                         source_entry->d_name);
                if (cleanup_incomplete &&
                    (has_suffix(source_entry->d_name, ".pxadb-part") ||
                     validation == PXA_STATUS_NOT_FOUND || validation == PXA_STATUS_DENIED ||
                     validation == PXA_STATUS_INVALID_ARGUMENT || validation == PXA_STATUS_PROTOCOL_ERROR) &&
                    remove_staging_source(source) == PXA_STATUS_OK) {
                    closedir(directory);
                    directory = opendir(g_store->inbox_root);
                    if (directory == NULL) return 0;
                }
            }
        }
        if (closedir(directory) != 0) {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Close Package Inbox failed: errno=%d", errno);
            return 0;
        }
    }

    for (index = g_store->entry_count; index != 0; --index) {
        const pxa_esp_package_entry_t *entry = &g_store->entries[index - 1u];
        if (!entry->installed && !entry->staged) remove_entry_at(index - 1u);
    }
    return 1;
}

static int path_is_within(const char *path, const char *directory) {
    size_t length;
    if (path == NULL || directory == NULL) return 0;
    length = strlen(directory);
    return strncmp(path, directory, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static const char *path_basename(const char *path) {
    const char *separator = strrchr(path, '/');
    return separator == NULL ? path : separator + 1;
}

static int reserve_inbox_destination(const char *name, char *destination,
                                     size_t destination_capacity) {
    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        int path_size;
        int descriptor;
        if (attempt == 0) {
            path_size = snprintf(destination, destination_capacity, "%s/%s",
                                 g_store->inbox_root, name);
        } else {
            path_size = snprintf(destination, destination_capacity, "%s/%u-%s",
                                 g_store->inbox_root, attempt, name);
        }
        if (path_size < 0 || (size_t)path_size >= destination_capacity) return 0;
        descriptor = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor >= 0) {
            close(descriptor);
            return 1;
        }
        if (errno != EEXIST) return 0;
    }
    return 0;
}

static int inbox_source_is_staged(const char *source) {
    for (size_t index = 0; index < g_store->entry_count; ++index) {
        const pxa_esp_package_entry_t *entry = &g_store->entries[index];
        if (entry->staged && strcmp(entry->inbox_root, source) == 0) return 1;
    }
    return 0;
}

bool pxa_esp_package_store_stage_file(const char *source_path) {
    char destination[PXA_ESP_PACKAGE_MAX_PATH];
    const char *name;
    int success = 0;

    if (!store_is_initialized() || source_path == NULL ||
        !path_is_within(source_path, CONFIG_PXA_MOUNT_POINT) ||
        !is_regular_file(source_path)) {
        return false;
    }
    name = path_basename(source_path);
    if (name[0] == '\0' || !has_suffix(name, ".pxa")) return false;
    if (!take_lock(g_store->transaction_lock)) return false;
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }

    if (path_is_within(source_path, g_store->inbox_root)) {
        success = scan_inbox(0) && inbox_source_is_staged(source_path);
        goto done;
    }
    /* Verify before moving the user-visible source into the private Inbox. */
    if (!stage_inbox_entry(source_path, NULL, NULL) ||
        !inbox_source_is_staged(source_path) ||
        !reserve_inbox_destination(name, destination, sizeof(destination))) {
        (void)scan_inbox(0);
        goto done;
    }
    if (rename(source_path, destination) != 0) {
        unlink(destination);
        (void)scan_inbox(0);
        goto done;
    }
    success = scan_inbox(0) && inbox_source_is_staged(destination);

done:
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success != 0;
}

static int legacy_owner_matches(
    const char *app_id,
    const uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES]) {
    static const uint8_t owner_magic[8] = {'P', 'X', 'A', 'O', 1, 0, 0, 0};
    uint8_t owner[8 + PXA_PACKAGE_DIGEST_BYTES];
    char path[PXA_ESP_PACKAGE_MAX_PATH];
    struct stat metadata;
    FILE *file;
    size_t count;
    if (snprintf(path, sizeof(path), "%s/owners/%s", g_store->state_root,
                 app_id) < 0 ||
        stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size != (off_t)sizeof(owner)) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    count = fread(owner, 1, sizeof(owner), file);
    fclose(file);
    return count == sizeof(owner) &&
           memcmp(owner, owner_magic, sizeof(owner_magic)) == 0 &&
           memcmp(owner + sizeof(owner_magic), publisher_root,
                  PXA_PACKAGE_DIGEST_BYTES) == 0;
}

static void migrate_legacy_permissions(
    const char *app_id, const char *canonical,
    const pxa_package_manifest_t *manifest) {
    uint16_t index;
    for (index = 0; index < manifest->permission_count; ++index) {
        const pxa_package_permission_t *permission =
            &manifest->permissions[index];
        pxa_permission_decision_t decision = PXA_PERMISSION_DENY;
        if (pxa_esp_permission_store_load(
                NULL,
                (pxa_bytes_t){(const uint8_t *)app_id, strlen(app_id)},
                permission->name, permission->scope,
                &decision) == PXA_STATUS_OK &&
            decision == PXA_PERMISSION_ALLOW) {
            (void)pxa_esp_permission_store_save(
                NULL,
                (pxa_bytes_t){(const uint8_t *)canonical, strlen(canonical)},
                permission->name, permission->scope, decision);
        }
    }
}

static int migrate_legacy_package(
    const char *legacy_root, const char *app_id,
    const uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES],
    const char *canonical, const char *storage_key,
    pxa_posix_installer_result_t *result) {
    pxa_posix_installer_identity_t expected;
    pxa_posix_install_disposition_t disposition;
    pxa_package_manifest_t *manifest = NULL;
    char installed_root[PXA_ESP_PACKAGE_MAX_PATH];
    char legacy_data[PXA_ESP_PACKAGE_MAX_PATH];
    char composite_data[PXA_ESP_PACKAGE_MAX_PATH];
    char legacy_owner[PXA_ESP_PACKAGE_MAX_PATH];
    int owner_matches = legacy_owner_matches(app_id, publisher_root);
    int old_data_exists;
    int path_size;
    pxa_status_t status;

    path_size = snprintf(legacy_data, sizeof(legacy_data), "%s/%s",
                         g_store->data_root, app_id);
    if (path_size < 0 || (size_t)path_size >= sizeof(legacy_data)) return 0;
    path_size = snprintf(composite_data, sizeof(composite_data), "%s/%s",
                         g_store->data_root, storage_key);
    if (path_size < 0 || (size_t)path_size >= sizeof(composite_data)) return 0;
    path_size = snprintf(legacy_owner, sizeof(legacy_owner), "%s/owners/%s",
                         g_store->state_root, app_id);
    if (path_size < 0 || (size_t)path_size >= sizeof(legacy_owner)) return 0;

    result->manifest = &manifest;
    result->root = installed_root;
    result->root_capacity = sizeof(installed_root);
    expected.publisher_key_id =
        (pxa_bytes_t){publisher_root, PXA_PACKAGE_DIGEST_BYTES};
    expected.app_id =
        (pxa_bytes_t){(const uint8_t *)app_id, strlen(app_id)};
    status = pxa_posix_installer_install_for_identity(
        g_store->installer, legacy_root, &expected, result, &disposition);
    if (status != PXA_STATUS_OK || manifest == NULL) return 0;

    old_data_exists = is_directory(legacy_data);
    if (old_data_exists && owner_matches) {
        if (is_directory(composite_data) || rename(legacy_data, composite_data) != 0) {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Retaining conflicting legacy private data for %s",
                     canonical);
            return 0;
        }
    } else if (old_data_exists) {
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                 "Quarantined unowned legacy private data for %s", app_id);
    }

    migrate_legacy_permissions(app_id, canonical, manifest);
    if (pxa_esp_package_policy_is_disabled(app_id)) {
        if (pxa_esp_package_policy_set_enabled(canonical, false))
            (void)pxa_esp_package_policy_set_enabled(app_id, true);
    }
    if (pxa_posix_fs_remove_tree(legacy_root) != PXA_STATUS_OK) return 0;
    if (owner_matches && !is_directory(legacy_data)) {
        (void)unlink(legacy_owner);
    }
    ESP_LOGI(PXA_ESP_PACKAGE_TAG, "Migrated Package identity %s", canonical);
    return refresh_entry_from_manifest(app_id, installed_root, 0, manifest);
}

static void scan_installed(void) {
    DIR *directory;
    struct dirent *entry;
    g_store->entry_count = 0;
    directory = opendir(g_store->packages_root);
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        char path[PXA_ESP_PACKAGE_MAX_PATH];
        char app_id[PXA_ESP_PACKAGE_MAX_ID];
        char canonical[PXA_ESP_PACKAGE_MAX_IDENTITY];
        char storage_key[PXA_ESP_PACKAGE_MAX_IDENTITY];
        char verified_root[PXA_ESP_PACKAGE_MAX_PATH];
        char manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
        uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
        pxa_posix_installer_result_t result;
        pxa_package_manifest_t *manifest = NULL;
        int path_size;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        path_size = snprintf(path, sizeof(path), "%s/%s",
                             g_store->packages_root, entry->d_name);
        if (path_size < 0 || (size_t)path_size >= sizeof(path) ||
            !is_directory(path) ||
            strncmp(entry->d_name, ".session-", 9) == 0) continue;
        memset(&result, 0, sizeof(result));
        result.struct_size = sizeof(result);
        result.manifest = &manifest;
        result.root = verified_root;
        result.root_capacity = sizeof(verified_root);
        path_size = snprintf(manifest_path, sizeof(manifest_path),
                             "%s/manifest.pxm", path);
        if (path_size < 0 || (size_t)path_size >= sizeof(manifest_path))continue;
        if(!is_regular_file(manifest_path))continue;
        if (!parse_manifest_file(manifest_path, &manifest) ||
            manifest == NULL || manifest->app_id.size == 0 ||
            manifest->app_id.size >= sizeof(app_id) ||
            !manifest_publisher_root(manifest, publisher_root)) {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Ignoring invalid installed Package %s",
                     entry->d_name);
            continue;
        }
        memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
        app_id[manifest->app_id.size] = '\0';
        if (!filesystem_identity(storage_key, sizeof(storage_key),
                                 publisher_root, app_id) ||
            !canonical_identity(canonical, sizeof(canonical), publisher_root,
                                app_id)) {
            continue;
        }
        if (strcmp(entry->d_name, app_id) == 0) {
            (void)migrate_legacy_package(path, app_id, publisher_root,
                                         canonical, storage_key, &result);
        } else if (strcmp(entry->d_name, storage_key) == 0) {
            (void)refresh_entry_from_manifest(app_id, path, 0, manifest);
        } else {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Ignoring Package stored under mismatched identity %s",
                     entry->d_name);
        }
    }
    closedir(directory);
    for(size_t i=0;i<g_store->locators.app_count;++i) {
        const pxa_locator_app_t *a=&g_store->locators.apps[i];
        if((a->flags&PXA_LOCATOR_PACKAGE_LEGACY) && !(a->flags&PXA_LOCATOR_PACKAGE_MOVING))continue;
        char relative[130],path[PXA_ESP_PACKAGE_MAX_PATH],manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
        uint8_t root[32];pxa_package_manifest_t *manifest=NULL;
        if(pxa_app_locators_find(&g_store->locators,a->root,a->app_id,NULL,relative,sizeof(relative))!=PXA_STATUS_OK)continue;
        int n=snprintf(path,sizeof(path),"%s/%s",g_store->packages_root,relative);
        if(n<0 || (size_t)n>=sizeof(path))continue;
        n=snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.pxm",path);
        if(n<0 || (size_t)n>=sizeof(manifest_path) || !parse_manifest_file(manifest_path,&manifest) ||
           !manifest_matches_app_id(manifest,a->app_id) || !manifest_publisher_root(manifest,root) || memcmp(root,a->root,32))continue;
        (void)refresh_entry_from_manifest(a->app_id,path,0,manifest);
    }
}

static int reserve_publisher_keys(size_t required) {
    pxa_esp_mbedtls_publisher_key_t *resized;
    size_t capacity;
    size_t bytes;
    if (required <= g_store->trust_key_capacity) return 1;
    if (required > SIZE_MAX / sizeof(*g_store->trust_keys)) return 0;
    capacity = g_store->trust_key_capacity == 0
                   ? required
                   : g_store->trust_key_capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*g_store->trust_keys)) return 0;
    bytes = capacity * sizeof(*g_store->trust_keys);
    if (g_store->trust_keys == NULL) {
        resized = esp_alloc(bytes);
    } else {
        resized = heap_caps_realloc(g_store->trust_keys, bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resized == NULL) {
            resized = heap_caps_realloc(g_store->trust_keys, bytes,
                                        MALLOC_CAP_8BIT);
        }
    }
    if (resized == NULL) return 0;
    g_store->trust_keys = resized;
    g_store->trust_key_capacity = capacity;
    return 1;
}

static int append_publisher_keys(const PxaTrustedPublisherKey *keys,
                                 size_t count, size_t *stored) {
    size_t index;
    if (stored == NULL || (count != 0 && keys == NULL) ||
        count > SIZE_MAX - *stored ||
        !reserve_publisher_keys(*stored + count)) {
        return 0;
    }
    for (index = 0; index < count; ++index) {
        if (keys[index].spki_der == NULL || keys[index].spki_der_size == 0) {
            return 0;
        }
        g_store->trust_keys[*stored].spki = keys[index].spki_der;
        g_store->trust_keys[*stored].spki_size = keys[index].spki_der_size;
        ++*stored;
    }
    return 1;
}

static int ensure_publisher_keys(void) {
    const PxaTrustedPublisherKey *keys = NULL;
    size_t stored = 0;
    size_t count;
    if (pxa_platform_trusted_publishers != NULL) {
        count = pxa_platform_trusted_publishers(&keys);
        if (!append_publisher_keys(keys, count, &stored)) return 0;
    }
#if CONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY
    keys = NULL;
    count = pxa_bundled_development_publishers(&keys);
    if (!append_publisher_keys(keys, count, &stored)) return 0;
#endif
    memset(&g_store->trust, 0, sizeof(g_store->trust));
    g_store->trust.struct_size = sizeof(g_store->trust);
    g_store->trust.keys = g_store->trust_keys;
    g_store->trust.key_count = stored;
    g_store->trust_key_count = stored;
    if (stored == 0) {
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                 "No trusted publishers configured; Package installation is disabled");
    }
    return 1;
}

/* NVS atomically replaces the bounded registry blob. A failed/uncertain
 * commit disables locator use until reload; corrupt records never become an
 * empty registry that could reassign another application's directory. */
static pxa_status_t persist_locators(void *context,uint64_t generation,
    const pxa_locator_publisher_t *pubs,size_t np,const pxa_locator_app_t *apps,size_t na) {
    (void)context;
    uint8_t *bytes=esp_alloc(PXA_LOCATOR_ENCODED_MAX);size_t size=0;
    if(!bytes)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_status_t status=pxa_app_locators_encode(generation,pubs,np,apps,na,bytes,PXA_LOCATOR_ENCODED_MAX,&size);
    nvs_handle_t handle;
    if(status==PXA_STATUS_OK) {
        esp_err_t error=nvs_open("pxa_apps",NVS_READWRITE,&handle);
        if(error==ESP_OK) {
            error=nvs_set_blob(handle,"locators_v1",bytes,size);
            if(error==ESP_OK)error=nvs_commit(handle);
            nvs_close(handle);
        }
        if(error!=ESP_OK)status=PXA_STATUS_IO_ERROR;
    }
    heap_caps_free(bytes);return status;
}
static int locator_available(void *context,const char *publisher,const char *app) {
    (void)context;char path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];struct stat st;
    const char *roots[2]={g_store->packages_root,g_store->data_root};
    for(unsigned i=0;i<2;++i) {
        int n=app?snprintf(path,sizeof(path),"%s/%s/%s",roots[i],publisher,app):snprintf(path,sizeof(path),"%s/%s",roots[i],publisher);
        if(n<0 || (size_t)n>=sizeof(path) || lstat(path,&st)==0 || errno!=ENOENT)return 0;
    }
    return 1;
}
static bool initialize_locators(void) {
    pxa_app_locators_init(&g_store->locators,NULL,persist_locators);
    g_store->locators.available=locator_available;
    nvs_handle_t handle;size_t size=0;
    esp_err_t error=nvs_open("pxa_apps",NVS_READONLY,&handle);
    if(error==ESP_ERR_NVS_NOT_FOUND)return true;
    if(error!=ESP_OK)return false;
    error=nvs_get_blob(handle,"locators_v1",NULL,&size);
    if(error==ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);return true;}
    if(error!=ESP_OK || size<24 || size>PXA_LOCATOR_ENCODED_MAX){nvs_close(handle);return false;}
    uint8_t *bytes=esp_alloc(size);
    if(!bytes){nvs_close(handle);return false;}
    error=nvs_get_blob(handle,"locators_v1",bytes,&size);nvs_close(handle);
    pxa_status_t status=error==ESP_OK?pxa_app_locators_decode(&g_store->locators,bytes,size):PXA_STATUS_IO_ERROR;
    heap_caps_free(bytes);return status==PXA_STATUS_OK;
}
static pxa_status_t lookup_locator(void *context,const pxa_posix_installer_identity_t *identity,
    uint8_t kind,char *out,size_t capacity) {
    (void)context;char app_id[65];uint16_t flags=0;
    if(!identity || identity->publisher_key_id.size!=32 || !identity->publisher_key_id.data ||
       !identity->app_id.data || !identity->app_id.size || identity->app_id.size>64)return PXA_STATUS_INVALID_ARGUMENT;
    memcpy(app_id,identity->app_id.data,identity->app_id.size);app_id[identity->app_id.size]=0;
    pxa_status_t status=pxa_app_locators_find(&g_store->locators,identity->publisher_key_id.data,app_id,&flags,out,capacity);
    if(status==PXA_STATUS_OK && (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING)))return PXA_STATUS_BAD_STATE;
    if(status==PXA_STATUS_OK && (flags&(kind==PXA_POSIX_INSTALLER_PATH_PACKAGE?PXA_LOCATOR_PACKAGE_LEGACY:PXA_LOCATOR_DATA_LEGACY)))
        return PXA_STATUS_NOT_FOUND;
    return status;
}
static pxa_status_t legacy_layout_flags(const uint8_t root[32],const char *app_id,uint16_t *flags) {
    char key[130],path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    if(!filesystem_identity(key,sizeof(key),root,app_id))return PXA_STATUS_INVALID_ARGUMENT;
    *flags=PXA_LOCATOR_MEDIA_LEGACY; /* removable media may be absent today */
    const char *bases[2]={g_store->packages_root,g_store->data_root};
    for(unsigned i=0;i<2;++i) {
        int n=snprintf(path,sizeof(path),"%s/%s",bases[i],key);
        if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_RESOURCE_LIMIT;
        if(is_directory(path))*flags|=(uint16_t)(1u<<i);
        if(i==1) {
            key[64]=':';n=snprintf(path,sizeof(path),"%s/%s",bases[i],key);
            if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_RESOURCE_LIMIT;
            if(is_directory(path))*flags|=PXA_LOCATOR_DATA_LEGACY;
        }
    }
    return PXA_STATUS_OK;
}
static pxa_status_t verified_incoming_layout(void *context,
    const pxa_posix_installer_identity_t *identity,const char *incoming) {
    (void)context;
    char app_id[65],relative[130],manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    uint16_t flags=0;uint8_t app_key[32];
    memcpy(app_id,identity->app_id.data,identity->app_id.size);app_id[identity->app_id.size]=0;
    pxa_status_t status=pxa_app_locators_find(&g_store->locators,identity->publisher_key_id.data,app_id,&flags,relative,sizeof(relative));
    if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return status;
    int already_registered=status==PXA_STATUS_OK;
    pxa_package_manifest_t *manifest=NULL;
    int n=snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.pxm",incoming);
    if(n<0 || (size_t)n>=sizeof(manifest_path) || !parse_manifest_file(manifest_path,&manifest))return PXA_STATUS_DENIED;
    pxa_publisher_profile_t profile={0};
    const char *profile_path="assets/pxa-publisher.json";
    const pxa_package_file_t *file=pxa_package_file_find(manifest,(pxa_bytes_t){(const uint8_t *)profile_path,strlen(profile_path)});
    if(file) {
        uint8_t bytes[PXA_PUBLISHER_PROFILE_BYTES],hash[32];
        n=snprintf(manifest_path,sizeof(manifest_path),"%s/%s",incoming,profile_path);
        if(n<0 || (size_t)n>=sizeof(manifest_path) || file->size>sizeof(bytes))return PXA_STATUS_DENIED;
        int fd=open(manifest_path,O_RDONLY|O_NOFOLLOW);struct stat st;
        if(fd<0)return PXA_STATUS_DENIED;
        ssize_t count=read(fd,bytes,sizeof(bytes));
        int valid=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && count>=0 && (uint64_t)count==file->size && (uint64_t)st.st_size==file->size;
        close(fd);
        if(!valid || pxa_esp_mbedtls_sha256(bytes,(size_t)count,hash)!=PXA_STATUS_OK || memcmp(hash,file->sha256,32) ||
           pxa_publisher_profile_parse(bytes,(size_t)count,&profile)!=PXA_STATUS_OK)return PXA_STATUS_DENIED;
    } else {
        /* An old package without signed author metadata keeps its legacy
         * adapter. It must not reserve a generic handle for all future apps
         * signed by this root. An existing verified binding stays intact. */
        return PXA_STATUS_OK;
    }
    if(already_registered)return PXA_STATUS_OK;
    static const char domain[]="PXA-APPKEY-1";
    uint8_t key_input[sizeof(domain)-1+32+2+64];size_t prefix=sizeof(domain)-1;
    memcpy(key_input,domain,prefix);memcpy(key_input+prefix,identity->publisher_key_id.data,32);
    key_input[prefix+32]=(uint8_t)identity->app_id.size;key_input[prefix+33]=0;
    memcpy(key_input+prefix+34,app_id,identity->app_id.size);
    if(pxa_esp_mbedtls_sha256(key_input,prefix+34+identity->app_id.size,app_key)!=PXA_STATUS_OK)return PXA_STATUS_INTERNAL;
    /* Determine the legacy adapter before the catalog exists at cold recovery.
     * Ownership still comes from the verified manifest/installer owner record. */
    status=legacy_layout_flags(identity->publisher_key_id.data,app_id,&flags);
    if(status!=PXA_STATUS_OK)return status;
    return pxa_app_locators_reserve(&g_store->locators,PXA_LOCATOR_SELF_NAMESPACE,
        identity->publisher_key_id.data,profile.handle,identity->publisher_key_id.data,app_id,app_key,flags);
}

static bool initialization_failed(const char *stage) {
    ESP_LOGE(PXA_ESP_PACKAGE_TAG,
             "Package store initialization failed at %s: errno=%d", stage,
             errno);
    release_store();
    return false;
}

bool pxa_esp_package_store_initialize(void) {
    pxa_posix_installer_config_t config;
    pxa_status_t installer_status;
    size_t workspace_size;
    if (store_is_initialized()) return true;
    if (!allocate_store()) return false;
    snprintf(g_store->state_root, sizeof(g_store->state_root), "%s/%s",
             CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT);
    if (!ensure_directory_tree(CONFIG_PXA_STATE_ROOT)) {
        return initialization_failed("create-state-root");
    }
    snprintf(g_store->packages_root, sizeof(g_store->packages_root), "%s/%s/%s",
             CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
             CONFIG_PXA_PACKAGES_DIR);
    snprintf(g_store->data_root, sizeof(g_store->data_root), "%s/%s/%s",
             CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
             CONFIG_PXA_PRIVATE_DATA_DIR);
    snprintf(g_store->builtin_root, sizeof(g_store->builtin_root),
             "%s/%s", CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_BUILTIN_PACKAGE_ROOT);
    snprintf(g_store->inbox_root, sizeof(g_store->inbox_root), "%s/%s/%s",
             CONFIG_PXA_MOUNT_POINT, CONFIG_PXA_STATE_ROOT,
             CONFIG_PXA_INBOX_DIR);
    if (!ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PACKAGES_DIR)) {
        return initialization_failed("create-packages-root");
    }
    if (!ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PRIVATE_DATA_DIR)) {
        return initialization_failed("create-data-root");
    }
    if (!ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_INBOX_DIR)) {
        return initialization_failed("create-inbox-root");
    }
    if (!pxa_esp_package_policy_initialize()) {
        return initialization_failed("load-disabled-policy");
    }
    if (!initialize_locators())return initialization_failed("load-app-locators");
    if (!ensure_publisher_keys()) {
        return initialization_failed("load-publisher-keys");
    }
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.storage_root = g_store->state_root;
    config.trust = (pxa_openssl_trust_t){
        .struct_size = sizeof(pxa_openssl_trust_t),
        .keys = (const pxa_openssl_publisher_key_t *)g_store->trust.keys,
        .key_count = g_store->trust.key_count,
    };
    config.flags = PXA_POSIX_INSTALLER_FLAG_SKIP_LOCK |
                   PXA_POSIX_INSTALLER_FLAG_REPAIR_CORRUPT |
                   PXA_POSIX_INSTALLER_FLAG_COMPOSITE_IDENTITY;
    config.verify = pxa_esp_mbedtls_p256_verify;
    config.verify_context = &g_store->trust;
    config.resolve_locator=lookup_locator;
    config.incoming_ready=verified_incoming_layout;
    /* The parser and installer workspaces are allocated from ESP memory at
     * initialization. Retain the protocol defaults instead of adding a
     * smaller ESP catalog quota for manifest contents. */
    pxa_package_limits_init(&config.limits);
    g_store->limits = config.limits;
    g_store->installer_config = config;
    workspace_size = pxa_posix_installer_workspace_size(&config);
    if (workspace_size == 0) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG,
                 "Package store initialization failed at "
                 "size-installer-workspace: config rejected");
        return initialization_failed("size-installer-workspace");
    }
    g_store->installer_workspace = esp_alloc(workspace_size);
    if (g_store->installer_workspace == NULL) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG,
                 "Installer workspace allocation failed: size=%u",
                 (unsigned)workspace_size);
        return initialization_failed("allocate-installer-workspace");
    }
    installer_status = pxa_posix_installer_init(
        g_store->installer_workspace, workspace_size, &config,
        &g_store->installer);
    if (installer_status != PXA_STATUS_OK) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG,
                 "Package store initialization failed at "
                 "initialize-installer: status=%d workspace=%u",
                 (int)installer_status, (unsigned)workspace_size);
        /* Installer initialization is an in-memory validation; errno would
         * only report an unrelated earlier filesystem call. */
        release_store();
        return false;
    }
    g_store->metadata_lock = xSemaphoreCreateMutex();
    g_store->transaction_lock = xSemaphoreCreateMutex();
    g_store->sync_lock = xSemaphoreCreateMutex();
    if (g_store->metadata_lock == NULL || g_store->transaction_lock == NULL ||
        g_store->sync_lock == NULL) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG, "Package store lock allocation failed");
        return initialization_failed("allocate-locks");
    }
    installer_status = pxa_posix_installer_recover_pending(g_store->installer);
    if (installer_status != PXA_STATUS_OK)
        ESP_LOGW(PXA_ESP_PACKAGE_TAG, "Some pending transactions need recovery: %d", (int)installer_status);
    (void)pxa_posix_installer_discard_transfers(g_store->installer);
    (void)unlink(CONFIG_PXA_MOUNT_POINT "/.pxadb-upload");
    scan_installed();
    (void)scan_inbox(1);
    g_store->initialized = 1;
    for(size_t i=0;i<g_store->entry_count;++i) {
        pxa_esp_package_entry_t *entry=&g_store->entries[i];char relative[130],root[PXA_ESP_PACKAGE_MAX_PATH];uint16_t flags=0;
        if(pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative))==PXA_STATUS_OK &&
           (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING|PXA_LOCATOR_MEDIA_MOVING))) {
            pxa_status_t recovered=pxa_esp_package_store_migrate_storage(entry->identity,root,sizeof(root));
            if(recovered!=PXA_STATUS_OK)ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Locator recovery pending for %s: %ld",entry->identity,(long)recovered);
            if(!g_store->locators.ready)return initialization_failed("recover-app-locators");
        }
    }
    ESP_LOGI(PXA_ESP_PACKAGE_TAG, "Package store ready (%d installed)",
             (int)g_store->entry_count);
    return true;
}

static void scan_builtins(size_t *available, size_t *rejected) {
    DIR *directory;
    struct dirent *entry;
    uint32_t generation;
    size_t index;
    if (available != NULL) *available = 0;
    if (rejected != NULL) *rejected = 0;
    if (!take_lock(g_store->metadata_lock)) return;
    generation = ++g_store->builtin_scan_generation;
    if (generation == 0) {
        generation = ++g_store->builtin_scan_generation;
    }
    release_lock(g_store->metadata_lock);
    directory = opendir(g_store->builtin_root);
    if (directory == NULL) {
        if (errno != ENOENT) {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Open built-in Package directory failed: errno=%d", errno);
        }
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        pxa_esp_package_entry_t candidate;
        char source[PXA_ESP_PACKAGE_MAX_PATH];
        int path_size;
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        path_size = snprintf(source, sizeof(source), "%s/%s",
                             g_store->builtin_root, entry->d_name);
        if (path_size < 0 || (size_t)path_size >= sizeof(source)) {
            if (rejected != NULL) ++*rejected;
            continue;
        }
        /* The built-in root may contain documentation alongside package
         * directories; only package directories participate in sync. */
        if (!is_directory(source)) continue;
        if (!safe_app_id(entry->d_name)) {
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                     "Skip built-in Package %s (not a valid app directory)",
                     entry->d_name);
            if (rejected != NULL) ++*rejected;
            continue;
        }
        if (!load_builtin_candidate(entry->d_name, &candidate)) {
            if (rejected != NULL) ++*rejected;
            continue;
        }
        if (!take_lock(g_store->metadata_lock)) {
            if (rejected != NULL) ++*rejected;
            continue;
        }
        {
            pxa_esp_package_entry_t *existing = find_entry(candidate.identity);
            if (existing != NULL && existing->installed &&
                !existing->built_in) {
                release_lock(g_store->metadata_lock);
                if (available != NULL) ++*available;
                continue;
            }
            existing = reserve_entry(entry->d_name,
                                     candidate.publisher_key_id);
            if (existing == NULL) {
                release_lock(g_store->metadata_lock);
                if (rejected != NULL) ++*rejected;
                continue;
            }
            candidate.enabled =
                !pxa_esp_package_policy_is_disabled(candidate.identity);
            candidate.builtin_scan_generation = generation;
            if (existing->staged) {
                candidate.staged = 1;
                candidate.staged_assets_available =
                    existing->staged_assets_available;
                candidate.has_staged_release_sequence =
                    existing->has_staged_release_sequence;
                candidate.staged_release_sequence =
                    existing->staged_release_sequence;
                candidate.staged_lineage_count =
                    existing->staged_lineage_count;
                snprintf(candidate.staged_version,
                         sizeof(candidate.staged_version), "%s",
                         existing->staged_version);
                snprintf(candidate.staged_icon_path,
                         sizeof(candidate.staged_icon_path), "%s",
                         existing->staged_icon_path);
                snprintf(candidate.inbox_root, sizeof(candidate.inbox_root),
                         "%s", existing->inbox_root);
            }
            *existing = candidate;
        }
        release_lock(g_store->metadata_lock);
        if (available != NULL) ++*available;
    }
    closedir(directory);

    if (!take_lock(g_store->metadata_lock)) return;
    for (index = g_store->entry_count; index != 0; --index) {
        pxa_esp_package_entry_t *existing = &g_store->entries[index - 1u];
        if (!existing->built_in ||
            existing->builtin_scan_generation == generation) {
            continue;
        }
        existing->built_in = 0;
        existing->installed = 0;
        existing->root[0] = '\0';
        if (!existing->staged) remove_entry_at(index - 1u);
    }
    release_lock(g_store->metadata_lock);
}

static void sync_builtins_locked(void) {
    size_t available = 0;
    size_t rejected = 0;
    int64_t sync_started_us = esp_timer_get_time();
    if (!take_lock(g_store->transaction_lock)) return;
    /* Each catalog item measures its manifest before allocating a private
     * workspace, so scanning does not reserve the protocol maximum. */
    scan_builtins(&available, &rejected);
    release_lock(g_store->transaction_lock);
    {
        pxa_esp_package_store_sync_fn callback = NULL;
        void *user_data = NULL;
        if (take_lock(g_store->metadata_lock)) {
            callback = g_store->sync_callback;
            user_data = g_store->sync_callback_user_data;
            release_lock(g_store->metadata_lock);
        }
        if (callback != NULL) callback(user_data);
    }
    ESP_LOGI(PXA_ESP_PACKAGE_TAG,
             "Built-in Package catalog scan: available=%u rejected=%u in %u ms",
             (unsigned)available, (unsigned)rejected,
             (unsigned)((esp_timer_get_time() - sync_started_us) / 1000));
}

void pxa_esp_package_store_set_sync_callback(
    pxa_esp_package_store_sync_fn callback, void *user_data) {
    if (!store_is_initialized() || !take_lock(g_store->metadata_lock)) return;
    g_store->sync_callback = callback;
    g_store->sync_callback_user_data = user_data;
    release_lock(g_store->metadata_lock);
}

void pxa_esp_package_store_sync_builtins(void) {
    if (!store_is_initialized() || !take_lock(g_store->sync_lock)) return;
    sync_builtins_locked();
    release_lock(g_store->sync_lock);
}

static void builtin_sync_task(void *arg) {
    (void)arg;
    pxa_esp_package_store_sync_builtins();
    if (take_lock(g_store->metadata_lock)) {
        g_store->sync_task = NULL;
        release_lock(g_store->metadata_lock);
    }
    vTaskDeleteWithCaps(NULL);
}

bool pxa_esp_package_store_sync_builtins_async(void) {
    TaskHandle_t task = NULL;
    if (!store_is_initialized() || !take_lock(g_store->metadata_lock)) return false;
    if (g_store->sync_task != NULL) {
        release_lock(g_store->metadata_lock);
        return true;
    }
    if (xTaskCreateWithCaps(builtin_sync_task, "pxa_pkg_sync",
                            PXA_ESP_PACKAGE_SYNC_TASK_STACK_SIZE, NULL,
                            PXA_ESP_PACKAGE_SYNC_TASK_PRIORITY, &task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        release_lock(g_store->metadata_lock);
        ESP_LOGE(PXA_ESP_PACKAGE_TAG, "Built-in Package sync task creation failed");
        return false;
    }
    g_store->sync_task = task;
    release_lock(g_store->metadata_lock);
    ESP_LOGI(PXA_ESP_PACKAGE_TAG, "Built-in Package sync scheduled");
    return true;
}

static bool resolve_identity_visit(const pxa_esp_package_record_t *record,void *context) {
    if(record->installed && record->has_publisher_root)
        pxa_package_identity_consider(context,record->id,record->app_id,record->publisher_root);
    return true;
}
pxa_status_t pxa_esp_package_store_resolve_identity(const char *query,
    uint8_t publisher_root[32],char *identity,size_t identity_capacity,
    char *app_id,size_t app_id_capacity) {
    if(!query || !*query || !publisher_root || !identity || !identity_capacity ||
       !app_id || !app_id_capacity)return PXA_STATUS_INVALID_ARGUMENT;
    memset(publisher_root,0,32);identity[0]=app_id[0]=0;
    pxa_package_identity_match_t match={.query=query};
    pxa_esp_package_store_visit(PXA_ESP_PACKAGE_VIEW_MANAGED,resolve_identity_visit,&match);
    if(match.matches>1){ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Ambiguous App ID: %s",query);return PXA_STATUS_BAD_STATE;}
    if(!match.matches)return PXA_STATUS_NOT_FOUND;
    if(strlen(match.identity)>=identity_capacity || strlen(match.app_id)>=app_id_capacity)
        return PXA_STATUS_RESOURCE_LIMIT;
    memcpy(publisher_root,match.publisher_root,32);
    memcpy(identity,match.identity,strlen(match.identity)+1);
    memcpy(app_id,match.app_id,strlen(match.app_id)+1);
    return PXA_STATUS_OK;
}

bool pxa_esp_package_store_is_enabled(const char *identity) {
    int enabled;
    pxa_esp_package_entry_t *entry;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return false;
    }
    entry = find_entry(identity);
    enabled = entry != NULL &&
              !pxa_esp_package_policy_is_disabled(entry->identity);
    release_lock(g_store->metadata_lock);
    return enabled;
}

bool pxa_esp_package_store_set_enabled(const char *identity, bool enabled) {
    int success = 0;
    pxa_esp_package_entry_t *entry;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return false;
    }
    entry = find_entry(identity);
    if (entry == NULL || !entry->installed) goto done;
    if (!pxa_esp_package_policy_set_enabled(entry->identity, enabled)) goto done;
    (void)refresh_existing_entry(entry->identity);
    success = 1;

done:
    release_lock(g_store->metadata_lock);
    return success;
}

bool pxa_esp_package_store_install(const char *identity) {
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) return false;
    const pxa_esp_package_entry_t *entry = find_entry(identity);
    bool already_builtin = entry != NULL && entry->installed && entry->built_in;
    release_lock(g_store->metadata_lock);
    /* Shell installation and PXADB deployment share one transaction and one
     * source lifetime. A provisioned built-in retains its existing behavior. */
    return already_builtin || pxa_esp_package_store_deploy(identity);
}

static bool install_file_for_owner(const char *source_path,const uint8_t *expected_root,const char *expected_app_id) {
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    char identity[PXA_ESP_PACKAGE_MAX_IDENTITY];
    char installed_root[PXA_ESP_PACKAGE_MAX_PATH];
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *manifest = NULL;
    pxa_posix_install_disposition_t disposition;
    pxa_status_t status;
    int success = 0;
    const char *name;
    if (!store_is_initialized() || source_path == NULL ||
        !path_is_within(source_path, CONFIG_PXA_MOUNT_POINT) ||
        !is_regular_file(source_path)) {
        return false;
    }
    name = path_basename(source_path);
    if (name[0] == '\0' || !has_suffix(name, ".pxa")) return false;
    if (!take_lock(g_store->transaction_lock)) return false;
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest = &manifest;
    result.root = installed_root;
    result.root_capacity = sizeof(installed_root);
    /* Inspect authenticated identity metadata without rereading the payload.
     * The transaction below still authenticates the entire container before
     * decoding, and is bound to this identity before any destination mutation. */
    if (read_source_into_store(source_path, &result, 1) != PXA_STATUS_OK ||
        manifest == NULL || manifest->app_id.size == 0 ||
        manifest->app_id.size >= sizeof(app_id)) {
        goto done;
    }
    memcpy(app_id, manifest->app_id.data, manifest->app_id.size);
    app_id[manifest->app_id.size] = '\0';
    if (!safe_app_id(app_id) ||
        !manifest_publisher_root(manifest, publisher_root) ||
        !canonical_identity(identity, sizeof(identity), publisher_root,
                            app_id)) {
        goto done;
    }
    if(expected_root && (!expected_app_id || memcmp(expected_root,publisher_root,32) || strcmp(expected_app_id,app_id)))goto done;
    {
        pxa_esp_package_entry_t *entry = find_entry(identity);
        if (entry != NULL && entry->installed && entry->built_in) goto done;
    }
    manifest = NULL;
    {
        pxa_posix_installer_identity_t expected = {
            {publisher_root, sizeof(publisher_root)},
            {(const uint8_t *)app_id, strlen(app_id)}};
        status = pxa_posix_installer_install_for_identity(
            g_store->installer, source_path, &expected, &result, &disposition);
        if (status != PXA_STATUS_OK)
            (void)pxa_posix_installer_recover(g_store->installer, &expected);
    }
    if (status == PXA_STATUS_OK && manifest_matches_app_id(manifest, app_id))
        success = refresh_entry_from_manifest(app_id, installed_root, 0,
                                              manifest);
done:
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success;
}

bool pxa_esp_package_store_install_file(const char *source_path) {
    return install_file_for_owner(source_path,NULL,NULL);
}
bool pxa_esp_package_store_install_file_for_identity(const char *source_path,const uint8_t root[32],const char *app_id) {
    return root && app_id && install_file_for_owner(source_path,root,app_id);
}

static bool publish_transfer_locked(const char *app_id) {
    char source[PXA_ESP_PACKAGE_MAX_PATH] = {0};
    char published[PXA_ESP_PACKAGE_MAX_PATH];
    pxa_posix_installer_result_t result = {0};
    pxa_package_manifest_t *manifest = NULL;
    bool success = false;
    int size = snprintf(source, sizeof(source), "%s/%s%s", g_store->inbox_root,
                        PXA_POSIX_INSTALLER_RECEIVING_PREFIX, app_id);
    if (size < 0 || (size_t)size + 4u >= sizeof(source)) { source[0] = '\0'; goto done; }
    if (!is_directory(source)) strcat(source, ".pxa");
    result.struct_size = sizeof(result);
    result.manifest = &manifest;
    result.root = published;
    result.root_capacity = sizeof(published);
    /* Size the result workspace using signed metadata, then authenticate the
     * complete source exactly once at publication. */
    if (read_source_into_store(source, &result, 1) != PXA_STATUS_OK) goto done;
    if (pxa_posix_installer_publish_transfer(g_store->installer, app_id, &result) == PXA_STATUS_OK) {
        /* Publication can replace the previous staged source or switch it
         * between a directory and a container. Retire stale staging metadata
         * while preserving every installed identity and its private data. */
        for (size_t index = 0; index < g_store->entry_count; ++index) {
            pxa_esp_package_entry_t *entry = &g_store->entries[index];
            if (entry->staged &&
                (strcmp(entry->inbox_root, published) == 0 ||
                 (!is_directory(entry->inbox_root) && !is_regular_file(entry->inbox_root)))) {
                entry->staged = 0;
                entry->staged_assets_available = 0;
                entry->inbox_root[0] = '\0';
            }
        }
        success = stage_inbox_entry(published, manifest, NULL) != 0;
        if (!success) (void)remove_staging_source(published);
    }
 done:
    if (!success && source[0] != '\0') (void)remove_staging_source(source);
    return success;
}

bool pxa_esp_package_store_publish_transfer(const char *app_id) {
    if (!store_is_initialized() || !safe_app_id(app_id) ||
        !take_lock(g_store->transaction_lock)) return false;
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    bool success = publish_transfer_locked(app_id);
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success;
}

void pxa_esp_package_store_discard_transfers(void) {
    if (!store_is_initialized() || !take_lock(g_store->transaction_lock)) return;
    (void)pxa_posix_installer_discard_transfers(g_store->installer);
    release_lock(g_store->transaction_lock);
}

bool pxa_esp_package_store_preview_file(const char *source_path,
                                        pxa_esp_package_preview_t *preview) {
    char verified_root[PXA_ESP_PACKAGE_MAX_PATH];
    uint8_t publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *manifest = NULL;
    const char *name;
    int success = 0;
    if (preview == NULL) return false;
    memset(preview, 0, sizeof(*preview));
    if (!store_is_initialized() || source_path == NULL ||
        !path_is_within(source_path, CONFIG_PXA_MOUNT_POINT) ||
        !is_regular_file(source_path)) {
        return false;
    }
    name = path_basename(source_path);
    if (name[0] == '\0' || !has_suffix(name, ".pxa")) return false;
    if (!take_lock(g_store->transaction_lock)) return false;
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest = &manifest;
    result.root = verified_root;
    result.root_capacity = sizeof(verified_root);
    if (verify_source_into_store(source_path, &result) != PXA_STATUS_OK ||
        manifest == NULL || manifest->app_id.size == 0) {
        goto done;
    }
    copy_bytes_text(preview->app_id, sizeof(preview->app_id), manifest->app_id);
    if (!manifest_publisher_root(manifest, publisher_root) ||
        !canonical_identity(preview->id, sizeof(preview->id), publisher_root,
                            preview->app_id)) {
        goto done;
    }
    memcpy(preview->publisher_root, publisher_root,
           sizeof(preview->publisher_root));
    preview->has_publisher_root = true;
    copy_bytes_text(preview->name, sizeof(preview->name), manifest->name);
    copy_bytes_text(preview->version, sizeof(preview->version), manifest->version);
    copy_bytes_text(preview->description, sizeof(preview->description),
                    manifest->description);
    if (preview->id[0] == '\0' || preview->name[0] == '\0' ||
        preview->version[0] == '\0') {
        goto done;
    }
    for (size_t index = 0;
         index < manifest->permission_count &&
         preview->permission_count < PXA_ESP_PACKAGE_PREVIEW_PERMISSION_MAX;
         ++index) {
        const pxa_package_permission_t *permission = &manifest->permissions[index];
        pxa_esp_package_permission_info_t *target =
            &preview->permissions[preview->permission_count];
        copy_bytes_text(target->name, sizeof(target->name), permission->name);
        copy_scope_text(target->scope, sizeof(target->scope), permission->scope);
        if (target->name[0] == '\0') goto done;
        target->required = permission->required != 0;
        ++preview->permission_count;
    }
    success = 1;
done:
    if (!success) memset(preview, 0, sizeof(*preview));
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success != 0;
}

typedef struct {
    const PxaPublisherNamespaceIssuer *issuers;
    size_t count;
    pxa_esp_mbedtls_publisher_key_t selected;
    pxa_esp_mbedtls_trust_t trust;
} namespace_verification_t;
static pxa_status_t namespace_authority(void *context,const uint8_t key_id[32],uint8_t authority[32]) {
    namespace_verification_t *v=context;
    for(size_t i=0;i<v->count;++i) {
        const PxaPublisherNamespaceIssuer *issuer=&v->issuers[i];uint8_t hash[32];
        if(!issuer->spki || !issuer->spki_bytes || issuer->spki_bytes>128)continue;
        if(pxa_esp_mbedtls_sha256(issuer->spki,issuer->spki_bytes,hash)!=PXA_STATUS_OK)return PXA_STATUS_INTERNAL;
        if(memcmp(hash,key_id,32))continue;
        uint8_t any=0;for(unsigned j=0;j<32;++j)any|=issuer->authority_id[j];
        if(!any)return PXA_STATUS_DENIED;
        memcpy(authority,issuer->authority_id,32);
        v->selected=(pxa_esp_mbedtls_publisher_key_t){issuer->spki,issuer->spki_bytes};
        v->trust=(pxa_esp_mbedtls_trust_t){.struct_size=sizeof(v->trust),.keys=&v->selected,.key_count=1};
        return PXA_STATUS_OK;
    }
    return PXA_STATUS_DENIED;
}
static pxa_status_t namespace_signature(void *context,pxa_bytes_t key,pxa_bytes_t domain,pxa_bytes_t message,pxa_bytes_t signature) {
    namespace_verification_t *v=context;
    return pxa_esp_mbedtls_p256_verify(&v->trust,key,domain,message,signature);
}
pxa_status_t pxa_esp_package_store_register_namespace(const uint8_t *token,size_t bytes) {
    if(!store_is_initialized())return PXA_STATUS_BAD_STATE;
    if(!pxa_platform_trusted_namespace_issuers)return PXA_STATUS_UNAVAILABLE;
    namespace_verification_t verification={0};
    verification.count=pxa_platform_trusted_namespace_issuers(&verification.issuers);
    if(!verification.count)return PXA_STATUS_UNAVAILABLE;
    if(verification.count>16 || !verification.issuers)return PXA_STATUS_DENIED;
    time_t now=time(NULL);if(now<1577836800)return PXA_STATUS_UNAVAILABLE;
    pxa_publisher_binding_t binding;
    pxa_status_t status=pxa_publisher_binding_verify(token,bytes,(uint64_t)now,&verification,namespace_authority,namespace_signature,&binding);
    if(status!=PXA_STATUS_OK)return status;
    uint8_t input[sizeof(PXA_PUBLISHER_NAMESPACE_DOMAIN)-1+48],namespace_key[32],app_key[32];
    size_t prefix=sizeof(PXA_PUBLISHER_NAMESPACE_DOMAIN)-1;
    memcpy(input,PXA_PUBLISHER_NAMESPACE_DOMAIN,prefix);memcpy(input+prefix,binding.authority,32);memcpy(input+prefix+32,binding.namespace_id,16);
    status=pxa_esp_mbedtls_sha256(input,sizeof(input),namespace_key);if(status!=PXA_STATUS_OK)return status;
    static const char domain[]="PXA-APPKEY-1";
    uint8_t app_input[sizeof(domain)-1+34+64];prefix=sizeof(domain)-1;size_t length=strlen(binding.app_id);
    memcpy(app_input,domain,prefix);memcpy(app_input+prefix,binding.root,32);app_input[prefix+32]=(uint8_t)length;app_input[prefix+33]=0;
    memcpy(app_input+prefix+34,binding.app_id,length);
    status=pxa_esp_mbedtls_sha256(app_input,prefix+34+length,app_key);if(status!=PXA_STATUS_OK)return status;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)){release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    char identity[130];uint16_t flags=0;
    if(!canonical_identity(identity,sizeof(identity),binding.root,binding.app_id))status=PXA_STATUS_DENIED;
    else {
        status=legacy_layout_flags(binding.root,binding.app_id,&flags);
        if(status==PXA_STATUS_OK)status=pxa_app_locators_reserve(&g_store->locators,PXA_LOCATOR_VERIFIED_NAMESPACE,namespace_key,
            binding.profile.handle,binding.root,binding.app_id,app_key,flags);
    }
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
}

static pxa_status_t legacy_media_relative(const uint8_t root[32],const char *app_id,char out[75]) {
    static const char domain[]="PXA-APPKEY-1",hex[]="0123456789abcdef";
    uint8_t input[sizeof(domain)-1+34+64],hash[32];size_t prefix=sizeof(domain)-1,length=strlen(app_id);
    if(length>64)return PXA_STATUS_INVALID_ARGUMENT;
    memcpy(input,domain,prefix);memcpy(input+prefix,root,32);
    input[prefix+32]=(uint8_t)length;input[prefix+33]=0;memcpy(input+prefix+34,app_id,length);
    pxa_status_t status=pxa_esp_mbedtls_sha256(input,prefix+34+length,hash);
    if(status!=PXA_STATUS_OK)return status;
    memcpy(out,"PXA/media/",10);
    for(size_t i=0;i<32;++i){out[10+2*i]=hex[hash[i]>>4];out[11+2*i]=hex[hash[i]&15];}out[74]=0;
    return PXA_STATUS_OK;
}
static int migration_probe(void *context,const char *path) {
    (void)context;struct stat st;
    if(lstat(path,&st)==0)return S_ISDIR(st.st_mode)?1:-1;
    return errno==ENOENT?0:-1;
}
static pxa_status_t migration_move(void *context,const char *from,const char *to) {
    (void)context;
    char parent[384];if(strlen(to)>=sizeof(parent))return PXA_STATUS_RESOURCE_LIMIT;
    strcpy(parent,to);char *last=strrchr(parent,'/');if(!last)return PXA_STATUS_DENIED;*last=0;
    for(char *p=parent+1;;++p) {
        if(*p && *p!='/')continue;
        char saved=*p;*p=0;struct stat st;
        if((mkdir(parent,0700)!=0 && errno!=EEXIST) || lstat(parent,&st)!=0 || !S_ISDIR(st.st_mode)){*p=saved;return PXA_STATUS_IO_ERROR;}
        *p=saved;if(!saved)break;
    }
    if(migration_probe(NULL,to)!=0)return PXA_STATUS_DENIED;
    if(rename(from,to)!=0)return PXA_STATUS_IO_ERROR;
#ifndef ESP_PLATFORM
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd<0)return PXA_STATUS_IO_ERROR;
    int synced=fsync(fd);close(fd);if(synced)return PXA_STATUS_IO_ERROR;
    strcpy(parent,from);last=strrchr(parent,'/');*last=0;
    fd=open(parent,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return PXA_STATUS_IO_ERROR;
    synced=fsync(fd);close(fd);if(synced)return PXA_STATUS_IO_ERROR;
#endif
    return PXA_STATUS_OK;
}
static pxa_status_t migrate_media_locked(pxa_esp_package_entry_t *entry,const char *relative) {
    char mount[192],legacy[75],old_path[384],next_path[384];
    pxa_status_t status=pxa_esp_fs_roots_media_mount(mount,sizeof(mount));
    if(status!=PXA_STATUS_OK)return status;
    status=legacy_media_relative(entry->publisher_key_id,entry->id,legacy);
    if(status!=PXA_STATUS_OK)return status;
    int n=snprintf(old_path,sizeof(old_path),"%s/%s",mount,legacy);
    int m=snprintf(next_path,sizeof(next_path),"%s/PXA/media/%s",mount,relative);
    if(n<0 || m<0 || (size_t)n>=sizeof(old_path) || (size_t)m>=sizeof(next_path))return PXA_STATUS_RESOURCE_LIMIT;
    pxa_locator_migration_io_t io={NULL,migration_probe,migration_move};
    return pxa_locator_migrate(&g_store->locators,entry->publisher_key_id,entry->id,PXA_LOCATOR_MEDIA_LEGACY,old_path,next_path,&io);
}
/* Fixed-media provider calls this before creating its first grant. It never
 * moves private data or package files while an application is running. */
pxa_status_t pxa_esp_package_store_prepare_media(const char *identity) {
    if(!store_is_initialized() || !identity)return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)){release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    pxa_status_t status=PXA_STATUS_NOT_FOUND;pxa_esp_package_entry_t *entry=find_entry(identity);
    if(entry && entry->installed) {
        char relative[130];uint16_t flags=0;
        status=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
        if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;
        else if(status==PXA_STATUS_OK && (flags&3u))status=PXA_STATUS_BAD_STATE;
        else if(status==PXA_STATUS_OK && (flags&PXA_LOCATOR_MEDIA_LEGACY))status=migrate_media_locked(entry,relative);
    }
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
}

/* Only the activation owner calls this, after stopping the old app. Each move
 * is preceded by a durable intent; boot/relaunch can finish an interrupted move.
 * Legacy adapters remain available for packages without a publisher profile. */
pxa_status_t pxa_esp_package_store_migrate_storage(const char *identity,char *root_out,size_t capacity) {
    if(!store_is_initialized() || !identity || !root_out || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)){release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    pxa_esp_package_entry_t *entry=find_entry(identity);pxa_status_t status=PXA_STATUS_NOT_FOUND;
    if(!entry || !entry->installed)goto migration_done;
    uint16_t flags=0;char relative[130],old_key[130],old_path[384],next_path[384];
    status=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
    if(status==PXA_STATUS_NOT_FOUND){status=PXA_STATUS_OK;goto migration_copy;}
    if(status!=PXA_STATUS_OK)goto migration_done;
    if(!filesystem_identity(old_key,sizeof(old_key),entry->publisher_key_id,entry->id)){status=PXA_STATUS_DENIED;goto migration_done;}
    pxa_locator_migration_io_t io={NULL,migration_probe,migration_move};
    const char *bases[2]={g_store->packages_root,g_store->data_root};
    for(unsigned i=0;i<2;++i) {
        if(!(flags&(1u<<i)))continue;
        int n=snprintf(old_path,sizeof(old_path),"%s/%s",bases[i],old_key);
        int m=snprintf(next_path,sizeof(next_path),"%s/%s",bases[i],relative);
        if(n<0 || m<0 || (size_t)n>=sizeof(old_path) || (size_t)m>=sizeof(next_path)){status=PXA_STATUS_RESOURCE_LIMIT;goto migration_done;}
        if(i==1 && migration_probe(NULL,old_path)==0) {
            /* Very old runtimes used the canonical colon spelling. */
            char colon[384];n=snprintf(colon,sizeof(colon),"%s/%s",bases[i],entry->identity);
            if(n<0 || (size_t)n>=sizeof(colon)){status=PXA_STATUS_RESOURCE_LIMIT;goto migration_done;}
            if(migration_probe(NULL,colon)==1 && migration_move(NULL,colon,old_path)!=PXA_STATUS_OK){status=PXA_STATUS_IO_ERROR;goto migration_done;}
        }
        status=pxa_locator_migrate(&g_store->locators,entry->publisher_key_id,entry->id,(uint16_t)(1u<<i),old_path,next_path,&io);
        if(status!=PXA_STATUS_OK)goto migration_done;
        if(i==0) {
            if(strlen(next_path)>=sizeof(entry->root)){status=PXA_STATUS_RESOURCE_LIMIT;goto migration_done;}
            memcpy(entry->root,next_path,strlen(next_path)+1);
        }
        else snprintf(entry->storage_key,sizeof(entry->storage_key),"%s",relative);
    }
    if(flags&PXA_LOCATOR_MEDIA_LEGACY) {
        pxa_status_t media=migrate_media_locked(entry,relative);
        if(media!=PXA_STATUS_OK && media!=PXA_STATUS_UNAVAILABLE)
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Media migration deferred for %s: %ld",entry->identity,(long)media);
        /* Removable-media failures never prevent private/Flash use. A first
         * fixed-root open retries this journal before exposing any directory. */
        if(!g_store->locators.ready){status=media;goto migration_done;}
    }
    status=PXA_STATUS_OK;
migration_copy:
    {int n=snprintf(root_out,capacity,"%s",entry->root);if(n<0 || (size_t)n>=capacity)status=PXA_STATUS_RESOURCE_LIMIT;}
migration_done:
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);
    if(status!=PXA_STATUS_OK)root_out[0]=0;
    return status;
}

pxa_status_t pxa_esp_package_store_storage_paths(const char *identity,
    char *private_root,size_t private_capacity,char *media_relative,size_t media_capacity) {
    if(!identity || !private_root || !media_relative || !store_is_initialized())return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->metadata_lock))return PXA_STATUS_UNAVAILABLE;
    pxa_status_t status=PXA_STATUS_NOT_FOUND;pxa_esp_package_entry_t *entry=find_entry(identity);
    if(entry && entry->installed && entry->has_publisher_key_id) {
        char relative[130];uint16_t flags=0;
        status=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
        if(status==PXA_STATUS_OK || status==PXA_STATUS_NOT_FOUND) {
            int n=snprintf(private_root,private_capacity,"%s/%s",g_store->data_root,entry->storage_key);
            if(n<0 || (size_t)n>=private_capacity){status=PXA_STATUS_RESOURCE_LIMIT;goto paths_done;}
            if(status==PXA_STATUS_OK)
                n=snprintf(media_relative,media_capacity,"PXA/media/%s",relative);
            else {
                char legacy[75];status=legacy_media_relative(entry->publisher_key_id,entry->id,legacy);
                if(status!=PXA_STATUS_OK)goto paths_done;
                n=snprintf(media_relative,media_capacity,"%s",legacy);
            }
            status=n<0 || (size_t)n>=media_capacity?PXA_STATUS_RESOURCE_LIMIT:PXA_STATUS_OK;
        }
    }
paths_done:
    release_lock(g_store->metadata_lock);
    if(status!=PXA_STATUS_OK){if(private_capacity)private_root[0]=0;if(media_capacity)media_relative[0]=0;}
    return status;
}

bool pxa_esp_package_store_deploy_detailed(
    const char *identity, pxa_esp_package_store_deploy_result_t *result_out) {
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    char source[PXA_ESP_PACKAGE_MAX_PATH] = {0};
    char installed_root[PXA_ESP_PACKAGE_MAX_PATH];
    uint8_t expected_publisher_root[PXA_PACKAGE_DIGEST_BYTES];
    pxa_posix_installer_identity_t expected_identity;
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *manifest = NULL;
    pxa_posix_install_disposition_t disposition;
    pxa_status_t status;
    pxa_status_t result_status = PXA_STATUS_BAD_STATE;
    const char *result_stage = "not_started";
    int source_size;
    int success = 0;
    int source_selected = 0;
    int transaction_lock_held = 0;
    int metadata_lock_held = 0;
    if (result_out != NULL) {
        result_out->status = result_status;
        result_out->stage = result_stage;
    }
    if (!store_is_initialized() || identity == NULL) {
        result_status = identity == NULL ? PXA_STATUS_INVALID_ARGUMENT
                                         : PXA_STATUS_BAD_STATE;
        result_stage = "store_unavailable";
        goto done;
    }
    transaction_lock_held = take_lock(g_store->transaction_lock);
    if (!transaction_lock_held) {
        result_status = PXA_STATUS_UNAVAILABLE;
        result_stage = "transaction_lock";
        goto done;
    }
    metadata_lock_held = take_lock(g_store->metadata_lock);
    if (!metadata_lock_held) {
        result_status = PXA_STATUS_UNAVAILABLE;
        result_stage = "metadata_lock";
        goto done;
    }
    if (safe_app_id(identity)) {
        int size = snprintf(source, sizeof(source), "%s/%s%s", g_store->inbox_root,
                            PXA_POSIX_INSTALLER_RECEIVING_PREFIX, identity);
        if (size < 0 || (size_t)size + 4u >= sizeof(source)) goto done;
        bool pending = is_directory(source);
        if (!pending) { strcat(source, ".pxa"); pending = is_regular_file(source); }
        if (pending && !publish_transfer_locked(identity)) {
            result_status = PXA_STATUS_DENIED;
            result_stage = "publish_transfer";
            goto done;
        }
    }
    {
        pxa_esp_package_entry_t *entry = find_entry(identity);
        if (entry == NULL || !entry->staged) {
            (void)scan_inbox(0);
            entry = find_entry(identity);
        }
        if (entry == NULL || !entry->staged ||
            (source_size = snprintf(source, sizeof(source), "%s",
                                    entry->inbox_root)) < 0 ||
            (size_t)source_size >= sizeof(source) ||
            (!is_directory(source) && !is_regular_file(source))) {
            result_status = PXA_STATUS_NOT_FOUND;
            result_stage = "staged_source";
            goto done;
        }
        source_selected = 1;
        snprintf(app_id, sizeof(app_id), "%s", entry->id);
        if (!entry->has_publisher_key_id) {
            result_status = PXA_STATUS_DENIED;
            result_stage = "publisher_identity";
            goto done;
        }
        memcpy(expected_publisher_root, entry->publisher_key_id,
               sizeof(expected_publisher_root));
    }
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest = &manifest;
    result.root = installed_root;
    result.root_capacity = sizeof(installed_root);
    if (!bind_store_result_buffers(&result)) {
        result_status = PXA_STATUS_RESOURCE_LIMIT;
        result_stage = "prepare_result";
        goto done;
    }
    manifest = NULL;
    expected_identity.publisher_key_id = (pxa_bytes_t){
        expected_publisher_root, sizeof(expected_publisher_root)};
    expected_identity.app_id = (pxa_bytes_t){
        (const uint8_t *)app_id, strlen(app_id)};
    /* PXADB owns the inbox directory until deploy returns. Adopt its files
     * into the normal recoverable transaction instead of duplicating them. */
    status = is_directory(source)
        ? pxa_posix_installer_adopt_staged_directory(
              g_store->installer, path_basename(source), &expected_identity,
              &result, &disposition)
        : pxa_posix_installer_install_for_identity(
              g_store->installer, source, &expected_identity, &result,
              &disposition);
    if (status != PXA_STATUS_OK) {
        (void)pxa_posix_installer_recover(g_store->installer, &expected_identity);
        result_status = status;
        result_stage = "install";
        goto done;
    }
    if (!manifest_matches_app_id(manifest, app_id)) {
        result_status = PXA_STATUS_DENIED;
        result_stage = "installed_manifest";
        goto done;
    }
    {
        uint8_t installed_publisher_root[PXA_PACKAGE_DIGEST_BYTES];
        if (manifest_publisher_root(manifest, installed_publisher_root) &&
            memcmp(installed_publisher_root, expected_publisher_root,
                   sizeof(expected_publisher_root)) == 0) {
            success = refresh_entry_from_manifest(app_id, installed_root, 0,
                                                  manifest);
        }
    }
    if (!success) {
        result_status = PXA_STATUS_DENIED;
        result_stage = "installed_identity";
        goto done;
    }
done:
    /* A deployment attempt consumes its selected transport source, including
     * admission or commit failures. Current and private data are separate. */
    if (source_selected) {
        (void)remove_staging_source(source);
        (void)scan_inbox(0);
    }
    if (metadata_lock_held) {
        release_lock(g_store->metadata_lock);
    }
    if (transaction_lock_held) {
        release_lock(g_store->transaction_lock);
    }
    if (success) {
        result_status = PXA_STATUS_OK;
        result_stage = "complete";
    } else {
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,
                 "Deployment failed: identity=%s stage=%s status=%ld",
                 identity != NULL ? identity : "(null)", result_stage,
                 (long)result_status);
    }
    if (result_out != NULL) {
        result_out->status = result_status;
        result_out->stage = result_stage;
    }
    return success;
}

bool pxa_esp_package_store_deploy(const char *identity) {
    return pxa_esp_package_store_deploy_detailed(identity, NULL);
}

static bool uninstall_package(const char *identity, bool keep_data) {
    pxa_posix_installer_identity_t identity_struct;
    pxa_posix_installer_result_t result;
    pxa_status_t status;
    pxa_package_manifest_t *manifest = NULL;
    pxa_esp_package_entry_t *entry;
    uint8_t id_bytes[65];
    uint8_t publisher_key_id[PXA_PACKAGE_DIGEST_BYTES];
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    char canonical[PXA_ESP_PACKAGE_MAX_IDENTITY];
    char installed_root[PXA_ESP_PACKAGE_MAX_PATH];
    char verified_root[PXA_ESP_PACKAGE_MAX_PATH];
    char data_path[PXA_ESP_PACKAGE_MAX_PATH];
    char storage_key[PXA_ESP_PACKAGE_MAX_IDENTITY];
    size_t manifest_size;
    size_t id_size;
    size_t index;
    bool cleanup_ok;
    bool clear_legacy;
    if (!store_is_initialized() || identity == NULL) return false;
    if (!take_lock(g_store->transaction_lock)) return false;
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    entry = find_entry(identity);
    if(entry) {
        char relative[130];uint16_t flags=0;
        pxa_status_t mapped=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
        if((mapped!=PXA_STATUS_OK && mapped!=PXA_STATUS_NOT_FOUND) || (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING))) {
            release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return false;
        }
    }
    if (entry == NULL || !entry->installed || entry->built_in ||
        !entry->has_publisher_key_id) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    memcpy(publisher_key_id, entry->publisher_key_id, sizeof(publisher_key_id));
    id_size = strlen(entry->id);
    if (id_size == 0 || id_size >= sizeof(id_bytes)) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    snprintf(installed_root, sizeof(installed_root), "%s", entry->root);
    snprintf(app_id, sizeof(app_id), "%s", entry->id);
    snprintf(canonical, sizeof(canonical), "%s", entry->identity);
    snprintf(storage_key, sizeof(storage_key), "%s", entry->storage_key);
    clear_legacy = legacy_owner_matches(app_id, publisher_key_id);
    if (storage_key[0] == '\0' ||
        snprintf(data_path, sizeof(data_path), "%s/%s", g_store->data_root,
                 storage_key) >= (int)sizeof(data_path)) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    manifest_size = manifest_size_at_root(installed_root);
    if (manifest_size == 0 || !ensure_encoded_capacity(manifest_size)) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    release_lock(g_store->metadata_lock);
    memset(&identity_struct, 0, sizeof(identity_struct));
    memcpy(id_bytes, app_id, id_size);
    id_bytes[id_size] = '\0';
    identity_struct.app_id = (pxa_bytes_t){id_bytes, id_size};
    identity_struct.publisher_key_id =
        (pxa_bytes_t){publisher_key_id, PXA_PACKAGE_DIGEST_BYTES};
    memset(&result, 0, sizeof(result));
    result.struct_size = sizeof(result);
    result.manifest_workspace = g_store->manifest_workspace;
    result.manifest_workspace_size = g_store->manifest_workspace_size;
    result.encoded = g_store->encoded;
    result.encoded_capacity = g_store->encoded_capacity;
    result.manifest = &manifest;
    result.root = verified_root;
    result.root_capacity = sizeof(verified_root);
    status = pxa_posix_installer_load_current(g_store->installer,
                                              &identity_struct, &result);
    {
        uint8_t manifest_root[PXA_PACKAGE_DIGEST_BYTES];
        if (status != PXA_STATUS_OK ||
            !manifest_matches_app_id(manifest, (const char *)id_bytes) ||
            !manifest_publisher_root(manifest, manifest_root) ||
            memcmp(manifest_root, publisher_key_id,
                   sizeof(publisher_key_id)) != 0) {
            release_lock(g_store->transaction_lock);
            return false;
        }
    }
    status = pxa_posix_installer_uninstall(g_store->installer,
                                           &identity_struct);
    if (status != PXA_STATUS_OK) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    cleanup_ok = true;
    if (!keep_data) {
        status = pxa_posix_fs_remove_tree(data_path);
        cleanup_ok = status == PXA_STATUS_OK || status == PXA_STATUS_NOT_FOUND;
        /* Also remove this verified app's pre-migration runtime namespace. */
        if (snprintf(data_path, sizeof(data_path), "%s/%s", g_store->data_root,
                     canonical) >= (int)sizeof(data_path)) {
            cleanup_ok = false;
        } else {
            status = pxa_posix_fs_remove_tree(data_path);
            if (status != PXA_STATUS_OK && status != PXA_STATUS_NOT_FOUND)
                cleanup_ok = false;
        }
        if (!cleanup_ok)
            ESP_LOGW(PXA_ESP_PACKAGE_TAG, "Private data cleanup failed for %s",
                     canonical);
        if (pxa_esp_permission_store_clear_app(
                (pxa_bytes_t){(const uint8_t *)canonical, strlen(canonical)},
                manifest->permissions, manifest->permission_count) != PXA_STATUS_OK)
            cleanup_ok = false;
        if (clear_legacy && pxa_esp_permission_store_clear_app(
                (pxa_bytes_t){id_bytes, id_size}, manifest->permissions,
                manifest->permission_count) != PXA_STATUS_OK)
            cleanup_ok = false;
    }
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    if (!keep_data && !pxa_esp_package_policy_set_enabled(canonical, true))
        cleanup_ok = false;
    for (index = 0; index < g_store->entry_count; ++index) {
        if (strcmp(g_store->entries[index].identity, canonical) == 0) {
            remove_entry_at(index);
            break;
        }
    }
    (void)scan_inbox(0);
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return cleanup_ok;
}

bool pxa_esp_package_store_uninstall(const char *identity) {
    return uninstall_package(identity, false);
}

bool pxa_esp_package_store_archive(const char *identity) {
    return uninstall_package(identity, true);
}

bool pxa_esp_package_store_clear_data(const char *identity) {
    char data_path[PXA_ESP_PACKAGE_MAX_PATH];
    char canonical[PXA_ESP_PACKAGE_MAX_IDENTITY];
    char storage_key[PXA_ESP_PACKAGE_MAX_IDENTITY];
    pxa_esp_package_entry_t *entry;
    pxa_status_t status;
    int path_size;
    int success = 0;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->transaction_lock)) {
        return false;
    }
    if (!take_lock(g_store->metadata_lock)) goto done;
    entry = find_entry(identity);
    if (entry == NULL || !entry->installed || entry->storage_key[0] == '\0') {
        release_lock(g_store->metadata_lock);
        goto done;
    }
    {char relative[130];uint16_t flags=0;
     pxa_status_t mapped=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
     if((mapped!=PXA_STATUS_OK && mapped!=PXA_STATUS_NOT_FOUND) || (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING))) {
         release_lock(g_store->metadata_lock);goto done;
     }}
    snprintf(canonical, sizeof(canonical), "%s", entry->identity);
    snprintf(storage_key, sizeof(storage_key), "%s", entry->storage_key);
    release_lock(g_store->metadata_lock);
    path_size = snprintf(data_path, sizeof(data_path), "%s/%s",
                         g_store->data_root, storage_key);
    if (path_size < 0 || (size_t)path_size >= sizeof(data_path)) goto done;
    status = pxa_posix_fs_remove_tree(data_path);
    if (status != PXA_STATUS_OK && status != PXA_STATUS_NOT_FOUND) goto done;
    /* Runtime versions before the namespace fix used the canonical colon
     * identity directly. Clear only this authenticated app's old directory. */
    path_size = snprintf(data_path, sizeof(data_path), "%s/%s",
                         g_store->data_root, canonical);
    if (path_size < 0 || (size_t)path_size >= sizeof(data_path)) goto done;
    status = pxa_posix_fs_remove_tree(data_path);
    if (status != PXA_STATUS_OK && status != PXA_STATUS_NOT_FOUND) goto done;
    if (!take_lock(g_store->metadata_lock)) goto done;
    (void)refresh_existing_entry(canonical);
    release_lock(g_store->metadata_lock);
    success = 1;

done:
    release_lock(g_store->transaction_lock);
    return success != 0;
}

bool pxa_esp_package_store_load_installed(
    const char *identity, char *root, size_t root_capacity,
    void *manifest_workspace, size_t manifest_workspace_size,
    uint8_t *encoded, size_t encoded_capacity,
    pxa_package_manifest_t **manifest) {
    pxa_esp_package_entry_t *entry;
    uint8_t publisher_key_id[PXA_PACKAGE_DIGEST_BYTES];
    char app_id[PXA_ESP_PACKAGE_MAX_ID];
    char package_root[PXA_ESP_PACKAGE_MAX_PATH];
    char manifest_path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
    size_t app_id_size;
    pxa_status_t status;
    if (!store_is_initialized() || identity == NULL || root == NULL ||
        manifest_workspace == NULL || encoded == NULL ||
        manifest == NULL) {
        return false;
    }
    *manifest = NULL;
    if (!take_lock(g_store->metadata_lock)) return false;
    entry = find_entry(identity);
    if (entry == NULL || !entry->installed || !entry->has_publisher_key_id) {
        release_lock(g_store->metadata_lock);
        return false;
    }
    app_id_size = strlen(entry->id);
    if (app_id_size == 0 || app_id_size >= PXA_ESP_PACKAGE_MAX_ID) {
        release_lock(g_store->metadata_lock);
        return false;
    }
    if (snprintf(package_root, sizeof(package_root), "%s", entry->root) >=
        (int)sizeof(package_root)) {
        release_lock(g_store->metadata_lock);
        return false;
    }
    memcpy(publisher_key_id, entry->publisher_key_id, sizeof(publisher_key_id));
    snprintf(app_id, sizeof(app_id), "%s", entry->id);
    release_lock(g_store->metadata_lock);
    if (root_capacity == 0 ||
        snprintf(root, root_capacity, "%s", package_root) >=
            (int)root_capacity) {
        return false;
    }
    if (!take_lock(g_store->transaction_lock)) return false;
    {
        int path_size = snprintf(manifest_path, sizeof(manifest_path),
                                 "%s/manifest.pxm", package_root);
        /* Managed Package storage is authenticated by the installer on every
         * install or update. Trusted Host storage only needs its manifest and
         * identity parsed when it is read. */
        status = path_size < 0 || (size_t)path_size >= sizeof(manifest_path) ||
                         !load_manifest_file(manifest_path, manifest_workspace,
                                             manifest_workspace_size, encoded,
                                             encoded_capacity, manifest)
                     ? PXA_STATUS_INVALID_ARGUMENT
                     : PXA_STATUS_OK;
        if (status == PXA_STATUS_OK &&
            !manifest_matches_app_id(*manifest, app_id)) {
            status = PXA_STATUS_DENIED;
        } else if (status == PXA_STATUS_OK) {
            uint8_t manifest_root[PXA_PACKAGE_DIGEST_BYTES];
            if (!manifest_publisher_root(*manifest, manifest_root) ||
                memcmp(manifest_root, publisher_key_id,
                       sizeof(publisher_key_id)) != 0) {
                status = PXA_STATUS_DENIED;
            }
        }
    }
    release_lock(g_store->transaction_lock);
    if (status != PXA_STATUS_OK || *manifest == NULL) {
        ESP_LOGE(PXA_ESP_PACKAGE_TAG,
                 "Load installed Package failed: id=%s status=%d workspace=%u",
                 identity, (int)status, (unsigned)manifest_workspace_size);
    }
    return status == PXA_STATUS_OK && *manifest != NULL;
}

size_t pxa_esp_package_store_manifest_workspace_size(void) {
    return store_is_initialized() ? g_store->manifest_workspace_size : 0;
}

size_t pxa_esp_package_store_installed_manifest_size(const char *identity) {
    pxa_esp_package_entry_t *entry;
    char root[PXA_ESP_PACKAGE_MAX_PATH];
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    entry = find_entry(identity);
    if (entry != NULL && entry->installed) {
        snprintf(root, sizeof(root), "%s", entry->root);
    } else {
        root[0] = '\0';
    }
    release_lock(g_store->metadata_lock);
    return manifest_size_at_root(root);
}

size_t pxa_esp_package_store_installed_manifest_workspace_size(
    const char *identity) {
    char root[PXA_ESP_PACKAGE_MAX_PATH];
    pxa_package_limits_t actual_limits;
    uint8_t *encoded = NULL;
    size_t encoded_size;
    size_t workspace_size = 0;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    {
        const pxa_esp_package_entry_t *entry = find_entry(identity);
        if (entry != NULL && entry->installed) {
            snprintf(root, sizeof(root), "%s", entry->root);
        } else {
            root[0] = '\0';
        }
    }
    release_lock(g_store->metadata_lock);
    encoded_size = manifest_size_at_root(root);
    if (encoded_size == 0 || (encoded = esp_alloc(encoded_size)) == NULL) {
        return 0;
    }
    {
        char path[PXA_ESP_PACKAGE_MAX_ASSET_PATH];
        int path_size = snprintf(path, sizeof(path), "%s/manifest.pxm", root);
        if (path_size < 0 || (size_t)path_size >= sizeof(path) ||
            !read_manifest_file(path, encoded, encoded_size, &encoded_size) ||
            !manifest_workspace_for_encoded(encoded, encoded_size,
                                            &actual_limits,
                                            &workspace_size)) {
            workspace_size = 0;
        }
    }
    heap_caps_free(encoded);
    return workspace_size;
}

bool pxa_esp_package_store_is_input_method(const char *identity) {
    if (!store_is_initialized() || !identity || !take_lock(g_store->metadata_lock)) return false;
    const pxa_esp_package_entry_t *entry = find_entry(identity);
    bool valid = entry && entry->installed && entry->enabled && entry->input_method;
    release_lock(g_store->metadata_lock);
    return valid;
}
pxa_status_t pxa_esp_package_store_begin_ime(const char *identity,
    pxa_runtime_t *runtime, const pxa_component_engine_t *engine,
    const pxa_package_activation_profile_t *services,
    const pxa_package_host_profile_t *host, pxa_posix_ime_provider_t **provider) {
    if (!store_is_initialized() || !identity || !provider) return PXA_STATUS_BAD_STATE;
    *provider=NULL;
    /* A concurrent installation must not stall the foreground runtime owner. */
    if (xSemaphoreTake(g_store->transaction_lock,0)!=pdTRUE) return PXA_STATUS_WOULD_BLOCK;
    pxa_status_t status = PXA_STATUS_NOT_FOUND;
    char root[PXA_ESP_PACKAGE_ROOT_BYTES] = {0};
    if (take_lock(g_store->metadata_lock)) {
        const pxa_esp_package_entry_t *entry = find_entry(identity);
        if (entry && entry->installed && entry->enabled && entry->input_method)
            snprintf(root, sizeof(root), "%s", entry->root);
        release_lock(g_store->metadata_lock);
    }
    if (*root) {
        status = pxa_posix_ime_provider_open_begin(g_store->installer, root, runtime,
            engine, services, host, provider);
    }
    if (status != PXA_STATUS_OK) release_lock(g_store->transaction_lock);
    return status;
}
pxa_status_t pxa_esp_package_store_step_ime(pxa_posix_ime_provider_t *provider,
    size_t budget, uint64_t (*next_instance)(void), pxa_component_t *component) {
    if (!provider || !pxa_posix_ime_provider_loading(provider) || !component || !budget || !next_instance)
        return PXA_STATUS_BAD_STATE;
    pxa_status_t status=pxa_posix_ime_provider_open_step(provider,budget);
    if (status==PXA_STATUS_WOULD_BLOCK) return status;
    if (status==PXA_STATUS_OK) {
        int64_t start=esp_timer_get_time();
        status=pxa_posix_ime_provider_start(provider,next_instance(),component);
        ESP_LOGI("PxaIme","activation status=%d us=%lld",status,(long long)(esp_timer_get_time()-start));
    }
    release_lock(g_store->transaction_lock);
    return status;
}
void pxa_esp_package_store_close_ime(pxa_posix_ime_provider_t *provider) {
    bool leased=pxa_posix_ime_provider_loading(provider);
    pxa_posix_ime_provider_close(provider);
    if (leased) release_lock(g_store->transaction_lock);
}

static void fill_package_record(const pxa_esp_package_entry_t *entry,
                                pxa_esp_package_record_t *record) {
    memset(record, 0, sizeof(*record));
    snprintf(record->id, sizeof(record->id), "%s", entry->identity);
    snprintf(record->app_id, sizeof(record->app_id), "%s", entry->id);
    snprintf(record->name, sizeof(record->name), "%s", entry->name);
    snprintf(record->version, sizeof(record->version), "%s", entry->version);
    if (entry->has_publisher_key_id) {
        memcpy(record->publisher_root, entry->publisher_key_id,
               sizeof(record->publisher_root));
        record->has_publisher_root = true;
    }
    record->built_in = entry->built_in != 0;
    record->installed = entry->installed != 0;
    record->staged = entry->staged != 0;
    record->enabled = entry->enabled != 0;
    record->has_private_data = entry->installed && entry->has_private_data;
    record->input_method = entry->installed && entry->input_method;
    if (entry->installed) {
        record->has_release_sequence = entry->has_release_sequence != 0;
        record->release_sequence = entry->release_sequence;
        record->permission_count = entry->permission_count;
        record->granted_permission_count = entry->granted_permission_count;
    }
    if (entry->staged) {
        snprintf(record->staged_version, sizeof(record->staged_version), "%s",
                 entry->staged_version);
        record->has_staged_release_sequence =
            entry->has_staged_release_sequence != 0;
        record->staged_release_sequence = entry->staged_release_sequence;
        if (entry->installed) {
            record->downgrade_warning =
                record->has_release_sequence &&
                (!record->has_staged_release_sequence ||
                 record->staged_release_sequence < record->release_sequence);
            record->signer_rollback_warning =
                entry->staged_lineage_count < entry->lineage_count;
        }
    }
}

static bool package_entry_matches_view(
    const pxa_esp_package_entry_t *entry, pxa_esp_package_view_t view) {
    if (view == PXA_ESP_PACKAGE_VIEW_RUNNABLE) {
        return entry->installed && entry->enabled;
    }
    return entry->installed || entry->staged;
}

static size_t count_package_records_locked(pxa_esp_package_view_t view) {
    size_t index;
    size_t count = 0;
    for (index = 0; index < g_store->entry_count; ++index) {
        if (package_entry_matches_view(&g_store->entries[index], view)) ++count;
    }
    return count;
}

static size_t visit_package_records_locked(
    pxa_esp_package_view_t view, pxa_esp_package_record_visitor_fn visitor,
    void *user_data) {
    size_t index;
    size_t count = 0;
    for (index = 0; index < g_store->entry_count; ++index) {
        const pxa_esp_package_entry_t *entry = &g_store->entries[index];
        pxa_esp_package_record_t record;
        if (!package_entry_matches_view(entry, view)) continue;
        fill_package_record(entry, &record);
        ++count;
        if (!visitor(&record, user_data)) break;
    }
    return count;
}

static bool valid_package_view(pxa_esp_package_view_t view) {
    return view == PXA_ESP_PACKAGE_VIEW_RUNNABLE ||
           view == PXA_ESP_PACKAGE_VIEW_MANAGED;
}

size_t pxa_esp_package_store_count(pxa_esp_package_view_t view) {
    size_t count;
    if (!store_is_initialized() || !valid_package_view(view) ||
        !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    count = count_package_records_locked(view);
    release_lock(g_store->metadata_lock);
    return count;
}

size_t pxa_esp_package_store_visit(
    pxa_esp_package_view_t view, pxa_esp_package_record_visitor_fn visitor,
    void *user_data) {
    size_t count;
    if (!store_is_initialized() || visitor == NULL ||
        !valid_package_view(view) || !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    count = visit_package_records_locked(view, visitor, user_data);
    release_lock(g_store->metadata_lock);
    return count;
}

bool pxa_esp_package_store_refresh_inbox(void) {
    int success;
    if (!store_is_initialized() || !take_lock(g_store->transaction_lock)) {
        return false;
    }
    if (!take_lock(g_store->metadata_lock)) {
        release_lock(g_store->transaction_lock);
        return false;
    }
    success = scan_inbox(0);
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success != 0;
}

bool pxa_esp_package_store_icon_source(
    const char *identity, pxa_esp_package_icon_variant_t variant,
    pxa_esp_package_icon_source_t *source) {
    const pxa_esp_package_entry_t *entry;
    const char *root = NULL;
    const char *relative_path = NULL;
    if (source == NULL) return false;
    memset(source, 0, sizeof(*source));
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return false;
    }
    entry = find_entry(identity);
    if (variant == PXA_ESP_PACKAGE_ICON_INSTALLED && entry != NULL &&
        entry->installed && entry->icon_path[0] != '\0') {
        root = entry->root;
        relative_path = entry->icon_path;
    } else if (variant == PXA_ESP_PACKAGE_ICON_STAGED && entry != NULL &&
               !entry->installed && entry->staged &&
               entry->staged_assets_available &&
               entry->staged_icon_path[0] != '\0') {
        root = entry->inbox_root;
        relative_path = entry->staged_icon_path;
    }
    if (root != NULL && relative_path != NULL) {
        snprintf(source->root, sizeof(source->root), "%s", root);
        snprintf(source->relative_path, sizeof(source->relative_path), "%s",
                 relative_path);
    }
    release_lock(g_store->metadata_lock);
    return root != NULL && relative_path != NULL;
}

size_t pxa_esp_package_store_permission_count(const char *identity) {
    const pxa_esp_package_entry_t *entry;
    size_t count = 0;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    entry = find_entry(identity);
    if (entry != NULL && (entry->installed || entry->staged)) {
        count = entry->permission_count;
    }
    release_lock(g_store->metadata_lock);
    return count;
}

size_t pxa_esp_package_store_visit_permissions(
    const char *identity, pxa_esp_package_permission_visitor_fn visitor,
    void *user_data) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    size_t count = 0;
    size_t index;
    if (!store_is_initialized() || identity == NULL || visitor == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return 0;
    }
    entry = find_entry(identity);
    if (!parse_entry_manifest(entry, &manifest)) {
        release_lock(g_store->metadata_lock);
        return 0;
    }
    for (index = 0; index < manifest->permission_count; ++index) {
        const pxa_package_permission_t *permission =
            &manifest->permissions[index];
        pxa_esp_package_permission_info_t info;
        memset(&info, 0, sizeof(info));
        copy_bytes_text(info.name, sizeof(info.name), permission->name);
        copy_scope_text(info.scope, sizeof(info.scope), permission->scope);
        info.required = permission->required != 0;
        info.granted = permission_granted(entry->identity, permission);
        ++count;
        if (!visitor(&info, user_data)) break;
    }
    release_lock(g_store->metadata_lock);
    return count;
}

bool pxa_esp_package_store_set_permission(const char *identity,
                                          size_t permission_index,
                                          bool granted) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    const pxa_package_permission_t *permission;
    pxa_status_t status;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return false;
    }
    entry = find_entry(identity);
    if (!parse_entry_manifest(entry, &manifest) ||
        permission_index >= manifest->permission_count) {
        release_lock(g_store->metadata_lock);
        return false;
    }
    permission = &manifest->permissions[permission_index];
    status = pxa_esp_permission_store_save(
               NULL,
               (pxa_bytes_t){(const uint8_t *)entry->identity,
                             strlen(entry->identity)},
               permission->name, permission->scope,
               granted ? PXA_PERMISSION_ALLOW : PXA_PERMISSION_DENY);
    if (status == PXA_STATUS_OK) {
        entry->granted_permission_count =
            granted_permission_count(entry->identity, manifest);
    }
    release_lock(g_store->metadata_lock);
    return status == PXA_STATUS_OK;
}

bool pxa_esp_package_store_refresh_permission_summary(const char *identity) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->metadata_lock)) {
        return false;
    }
    entry = find_entry(identity);
    if (!parse_entry_manifest(entry, &manifest)) {
        release_lock(g_store->metadata_lock);
        return false;
    }
    entry->permission_count =
        manifest->permission_count > UINT8_MAX
            ? UINT8_MAX
            : (uint8_t)manifest->permission_count;
    entry->granted_permission_count =
        granted_permission_count(entry->identity, manifest);
    release_lock(g_store->metadata_lock);
    return true;
}

pxa_posix_installer_t *pxa_esp_package_store_installer(void) {
    return g_store != NULL ? g_store->installer : NULL;
}

#endif /* ESP_PLATFORM */
