/* ESP32 package store on the C stack: catalogs product-owned built-ins and
 * installs verified Inbox Packages (posix adapter, no flock), persisting the
 * disabled policy in NVS. Replaces the legacy C++ package-store implementation. */

#include "pxa_esp_package_store.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_app_transfer.h"
#include "pxa_esp_legacy_adoption.h"
#include "pxa_package_identity_match.h"
#include "pxa_app_locators.h"
#include "pxa_publisher_profile.h"
#include "pxa_publisher_binding.h"
#include "pxa_locator_migration.h"
#include "pxa_private_data_move.h"
#include "pxa_esp_fs_roots.h"
#include "pxa_esp_package_policy.h"
#include "pxa/pxa_esp_storage.h"

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
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "esp_random.h"
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
#include "pxa_esp_package_trust.h"
#include "pxa_esp_installations.h"
#include "pxa_esp_install_approval.h"
#include "pxa_esp_resource_transfer.h"
#include "pxa_esp_volumes.h"

#if defined(__cplusplus)
extern "C" {
#endif
size_t pxa_platform_trusted_namespace_issuers(
    const PxaPublisherNamespaceIssuer **output_keys) __attribute__((weak));
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
    uint64_t mount_epoch,install_generation,data_generation;
    uint8_t install_id[16];
    uint8_t manifest_sha256[32];
    uint8_t volume_local,ambiguous,archived,managed_media;
    uint8_t permission_summary_known;
    uint64_t granted_permissions;
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
    uint8_t legacy_ready;
    pxa_app_locators_t locators;
    pxa_posix_installer_t *installer;
    void *installer_workspace;
    pxa_posix_installer_config_t installer_config;
    void *manifest_workspace;
    size_t manifest_workspace_size;
    uint8_t *encoded;
    size_t encoded_capacity;
    pxa_esp_mbedtls_trust_t trust;
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
    pxa_esp_installations_t *pending_volume_owner;
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
    if (required == 0 || required > PXA_ESP_SIGNED_MANIFEST_MAX_BYTES) return 0;
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
    if(!parse_manifest_file(manifest_path,manifest_out))return 0;
    if(entry->volume_local) {
        uint8_t hash[32];
        if(pxa_esp_mbedtls_sha256((*manifest_out)->encoded.data,(*manifest_out)->encoded.size,hash)!=PXA_STATUS_OK ||
            memcmp(hash,entry->manifest_sha256,32))return 0;
    }
    return 1;
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
        if(entry->volume_local)return;
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
        entry->volume_local?0:granted_permission_count(entry->identity, manifest);
    if(entry->volume_local) {entry->granted_permissions=0;entry->permission_summary_known=0;}
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

static bool package_entry_has_home(const pxa_esp_package_entry_t *entry) {
    return entry->installed || (entry->volume_local && entry->archived);
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
        if (!package_entry_has_home(entry) && !entry->staged)
            remove_entry_at(index - 1u);
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

    if (!store_is_initialized() || !g_store->legacy_ready || source_path == NULL ||
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

/* Compatibility metadata is optional garbage once a local record owns the
 * installation. Retry on catalog discovery, including after an interrupted
 * cleanup; Flash/NVS failures must never disable the new installation. */
static __attribute__((noinline)) void collect_legacy_locator(const pxa_posix_app_home_t *home) {
    if(!g_store->legacy_ready || !g_store->locators.ready)return;
    char relative[130],legacy[130],path[PXA_ESP_PACKAGE_MAX_PATH];uint16_t flags;
    pxa_status_t status=pxa_app_locators_find(&g_store->locators,home->record.publisher_root,
        home->record.app_id,&flags,relative,sizeof(relative));
    if(status!=PXA_STATUS_OK || (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING)))return;
    if(!filesystem_identity(legacy,sizeof(legacy),home->record.publisher_root,home->record.app_id))return;
    const char *locations[]={relative,legacy};
    const char *roots[]={g_store->packages_root,g_store->data_root};
    for(unsigned root=0;root<2;++root)for(unsigned i=0;i<2;++i) {
        int n=snprintf(path,sizeof(path),"%s/%s",roots[root],locations[i]);struct stat st;
        if(n<0 || (size_t)n>=sizeof(path) || !lstat(path,&st) || errno!=ENOENT)return;
    }
    /* A much older runtime used the canonical colon spelling for saves.
     * Retained orphan data also keeps its compatibility locator for recovery. */
    legacy[64]=':';int n=snprintf(path,sizeof(path),"%s/%s",g_store->data_root,legacy);struct stat st;
    if(n<0 || (size_t)n>=sizeof(path) || !lstat(path,&st) || errno!=ENOENT)return;
    status=pxa_app_locators_forget(&g_store->locators,home->record.publisher_root,home->record.app_id);
    if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Retired legacy locator retained: app=%s status=%ld",home->record.app_id,(long)status);
    char identity[PXA_ESP_PACKAGE_ID_BYTES];
    if(canonical_identity(identity,sizeof(identity),home->record.publisher_root,home->record.app_id) &&
        pxa_esp_package_policy_is_disabled(identity) && !pxa_esp_package_policy_set_enabled(identity,true))
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Retired legacy disabled policy retained: app=%s",home->record.app_id);
}
typedef struct {uint64_t epoch;size_t rejected;} volume_catalog_context_t;
static int catalog_volume_home(void *context,const pxa_posix_app_home_t *home) {
    volume_catalog_context_t *scan=context;pxa_package_manifest_t *manifest=NULL;
    char root[PXA_ESP_PACKAGE_MAX_PATH],path[PXA_ESP_PACKAGE_MAX_ASSET_PATH],identity[PXA_ESP_PACKAGE_MAX_IDENTITY];
    uint8_t hash[32],publisher[32];
    const int archived=!!(home->record.flags&PXA_INSTALL_RECORD_ARCHIVED);
    memcpy(publisher,home->record.publisher_root,32);
    if(!canonical_identity(identity,sizeof(identity),publisher,home->record.app_id)) {
        ++scan->rejected;return 1;
    }
    if(!archived && (pxa_posix_app_home_path(home,PXA_POSIX_APP_HOME_PACKAGE,root,sizeof(root))!=PXA_STATUS_OK ||
        snprintf(path,sizeof(path),"%s/manifest.pxm",root)>=(int)sizeof(path) ||
        !parse_manifest_file(path,&manifest) ||
        pxa_esp_mbedtls_sha256(manifest->encoded.data,manifest->encoded.size,hash)!=PXA_STATUS_OK ||
        memcmp(hash,home->record.manifest_sha256,32) ||
        !manifest_matches_app_id(manifest,home->record.app_id) ||
        !manifest_publisher_root(manifest,publisher) || memcmp(publisher,home->record.publisher_root,32))) {
        ++scan->rejected;return 1;
    }
    pxa_esp_package_entry_t *entry=find_entry(identity);
    if(entry && (entry->installed || entry->volume_local) && (!entry->volume_local || entry->mount_epoch!=scan->epoch)) {
        /* A logical identity is not an installation selector. Quarantine the
         * ambiguity until the Host explicitly resolves/migrates the instances. */
        entry->ambiguous=1;entry->enabled=0;++scan->rejected;return 1;
    }
    if(!entry)entry=reserve_entry(home->record.app_id,publisher);
    if(!entry) {++scan->rejected;return 0;}
    entry->volume_local=1;entry->mount_epoch=scan->epoch;entry->install_generation=home->record.generation;
    entry->data_generation=home->record.data_generation;
    entry->managed_media=!!(home->record.flags&PXA_INSTALL_RECORD_MANAGED_MEDIA);
    memcpy(entry->install_id,home->record.install_id,16);
    memcpy(entry->manifest_sha256,home->record.manifest_sha256,32);
    snprintf(entry->storage_key,sizeof(entry->storage_key),"%s",home->locator);
    if(!archived) {
        update_installed_metadata(entry,manifest);
        snprintf(entry->root,sizeof(entry->root),"%s",root);
    } else {
        entry->root[0]=0;
        /* A cold archive has no package metadata to localize. Keep its signed
         * App ID readable, including in move-progress notifications. */
        if(!entry->name[0])snprintf(entry->name,sizeof(entry->name),"%s",home->record.app_id);
    }
    entry->archived=archived;entry->installed=!archived;entry->built_in=0;
    entry->enabled=!archived && !!(home->record.flags&PXA_INSTALL_RECORD_ENABLED) && !entry->ambiguous;
    entry->has_private_data=1;collect_legacy_locator(home);return 1;
}
static void scan_volume_homes(void) {
    pxa_volume_info_t *volumes=esp_alloc(sizeof(*volumes)*PXA_ESP_STORAGE_MAX_VOLUMES);
    if(!volumes)return;
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        pxa_esp_installations_t *owner=NULL;size_t invalid=0;
        if(volumes[i].state!=PXA_VOLUME_READY || volumes[i].ambiguous)continue;
        pxa_status_t status=pxa_esp_installations_open(volumes[i].mount_epoch,&owner);
        volume_catalog_context_t scan={volumes[i].mount_epoch,0};
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_visit(owner,catalog_volume_home,&scan,&invalid);
        (void)pxa_esp_installations_close(&owner);
        if(status!=PXA_STATUS_OK || invalid || scan.rejected)
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Volume catalog: status=%ld invalid=%u rejected=%u",(long)status,(unsigned)invalid,(unsigned)scan.rejected);
    }
    heap_caps_free(volumes);
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
    *flags=0;
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
    /* Legacy Flash state is an optional compatibility adapter. A missing,
     * full or damaged primary filesystem cannot prevent discovering complete
     * applications on other volumes, and must never create an SD substitute
     * for the old per-app Flash registry. */
    pxa_app_locators_init(&g_store->locators,NULL,NULL);
    if(CONFIG_PXA_STORAGE_PARTITION_LABEL[0] && esp_littlefs_mounted(CONFIG_PXA_STORAGE_PARTITION_LABEL)) {
        g_store->legacy_ready=ensure_directory_tree(CONFIG_PXA_STATE_ROOT) &&
            ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PACKAGES_DIR) &&
            ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PRIVATE_DATA_DIR) &&
            ensure_directory_tree(CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_INBOX_DIR) &&
            pxa_esp_package_policy_initialize() && initialize_locators();
        if(!g_store->legacy_ready) {
            pxa_app_locators_init(&g_store->locators,NULL,NULL);
            ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Legacy primary store unavailable; volume-local installations remain independent");
        }
    }
    if (pxa_esp_package_installer_profile(&config,&g_store->trust)!=PXA_STATUS_OK)
        return initialization_failed("load-publisher-keys");
    config.storage_root = g_store->state_root;
    config.resolve_locator=g_store->legacy_ready?lookup_locator:NULL;
    config.incoming_ready=g_store->legacy_ready?verified_incoming_layout:NULL;
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
    if(g_store->legacy_ready) {
        installer_status = pxa_posix_installer_recover_pending(g_store->installer);
        if (installer_status != PXA_STATUS_OK)
            ESP_LOGW(PXA_ESP_PACKAGE_TAG, "Some pending transactions need recovery: %d", (int)installer_status);
        (void)pxa_posix_installer_discard_transfers(g_store->installer);
        (void)unlink(CONFIG_PXA_MOUNT_POINT "/.pxadb-upload");
        scan_installed();
        (void)scan_inbox(1);
    }
    scan_volume_homes();
    g_store->initialized = 1;
    for(size_t i=0;i<g_store->entry_count;++i) {
        pxa_esp_package_entry_t *entry=&g_store->entries[i];char relative[130],root[PXA_ESP_PACKAGE_MAX_PATH];uint16_t flags=0;
        if(!g_store->legacy_ready || entry->volume_local)continue;
        if(pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative))==PXA_STATUS_OK &&
           (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING))) {
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
    if (!store_is_initialized() || !g_store->legacy_ready || !take_lock(g_store->sync_lock)) return;
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
    if(store_is_initialized() && !g_store->legacy_ready)return true;
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
    enabled = entry != NULL && !entry->ambiguous && (entry->volume_local?entry->enabled:
              !pxa_esp_package_policy_is_disabled(entry->identity));
    release_lock(g_store->metadata_lock);
    return enabled;
}

bool pxa_esp_package_store_set_enabled(const char *identity, bool enabled) {
    int success = 0;
    pxa_esp_package_entry_t *entry;
    if (!store_is_initialized() || identity == NULL || !take_lock(g_store->transaction_lock))return false;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return false;}
    entry = find_entry(identity);
    if (entry == NULL || !entry->installed) goto done;
    if(entry->ambiguous)goto done;
    if(entry->volume_local) {
        pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home={0};
        pxa_status_t status=pxa_esp_installations_open(entry->mount_epoch,&owner);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_set_enabled(owner,entry->storage_key,entry->install_generation,enabled,&home);
        (void)pxa_esp_installations_close(&owner);
        if(home.record.generation) {entry->enabled=!!(home.record.flags&PXA_INSTALL_RECORD_ENABLED);entry->install_generation=home.record.generation;}
        success=status==PXA_STATUS_OK;
        goto done;
    }
    if (!pxa_esp_package_policy_set_enabled(entry->identity, enabled)) goto done;
    (void)refresh_existing_entry(entry->identity);
    success = 1;

done:
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
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
    if (!store_is_initialized() || !g_store->legacy_ready || source_path == NULL ||
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
    return pxa_esp_package_store_install_file_on_volume(source_path,0,NULL,NULL)==PXA_STATUS_OK;
}
bool pxa_esp_package_store_install_file_for_identity(const char *source_path,const uint8_t root[32],const char *app_id) {
    return root && app_id && pxa_esp_package_store_install_file_on_volume(source_path,0,root,app_id)==PXA_STATUS_OK;
}

static pxa_status_t retry_volume_cleanup(void) {
    return pxa_esp_installations_close(&g_store->pending_volume_owner);
}
static pxa_status_t close_volume_operation(pxa_esp_installations_t **owner,pxa_status_t status) {
    pxa_status_t closed=pxa_esp_installations_close(owner);
    if(*owner) {g_store->pending_volume_owner=*owner;*owner=NULL;}
    return status==PXA_STATUS_OK?closed:status;
}
static pxa_status_t retain_source_volume(const char *source,uint64_t *epoch) {
    pxa_volume_info_t volume;char physical[PXA_ESP_STORAGE_PATH_BYTES];*epoch=0;
    pxa_status_t status=pxa_esp_storage_resolve_volume(source,&volume);
    if(status!=PXA_STATUS_OK)return status;
    if(!(volume.capabilities&PXA_VOLUME_READ))return PXA_STATUS_DENIED;
    status=pxa_esp_storage_volume_path(volume.mount_epoch,physical,sizeof(physical));
    if(status!=PXA_STATUS_OK)return status;
    /* A temporary private VFS alias has a separate session lifetime. Package
     * sources use physical public volumes, not another app's leased alias. */
    if(!path_is_within(source,physical))return PXA_STATUS_DENIED;
    status=pxa_esp_volumes_retain(volume.mount_epoch);
    if(status==PXA_STATUS_OK)*epoch=volume.mount_epoch;
    return status;
}
static pxa_status_t installation_path_fits(uint64_t epoch,const char *locator) {
    char mount[PXA_ESP_STORAGE_PATH_BYTES],package[PXA_ESP_PACKAGE_ROOT_BYTES];
    pxa_status_t status=pxa_esp_storage_volume_path(epoch,mount,sizeof(mount));
    if(status!=PXA_STATUS_OK)return status;
    int length=snprintf(package,sizeof(package),"%s/PXA/apps/%s/package",mount,locator);
    return length<0 || (size_t)length>=sizeof(package)?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
static pxa_status_t default_install_volume(const pxa_package_manifest_t *manifest,uint64_t *epoch) {
    uint8_t publisher[32];char app[65],locator[PXA_POSIX_APP_LOCATOR_BYTES];
    if(!manifest_publisher_root(manifest,publisher) || !manifest->app_id.size || manifest->app_id.size>=sizeof(app))return PXA_STATUS_DENIED;
    copy_bytes_text(app,sizeof(app),manifest->app_id);
    pxa_status_t locator_status=pxa_esp_installations_locator(publisher,app,locator,sizeof(locator));
    if(locator_status!=PXA_STATUS_OK)return locator_status;
    pxa_volume_info_t *volumes=esp_alloc(sizeof(*volumes)*PXA_ESP_STORAGE_MAX_VOLUMES);
    if(!volumes)return PXA_STATUS_RESOURCE_LIMIT;
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);*epoch=0;
    pxa_status_t status=PXA_STATUS_UNAVAILABLE;
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        pxa_volume_info_t *volume=&volumes[i];pxa_esp_installations_t *owner=NULL;pxa_install_plan_t plan;
        const uint64_t required=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || (volume->capabilities&required)!=required)continue;
        if(manifest->package_kind!=PXA_PACKAGE_KIND_RESOURCES &&
            installation_path_fits(volume->mount_epoch,locator)!=PXA_STATUS_OK)continue;
        status=pxa_esp_installations_open(volume->mount_epoch,&owner);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_plan(owner,manifest,0,
            manifest->package_kind!=PXA_PACKAGE_KIND_RESOURCES,&plan);
        (void)pxa_esp_installations_close(&owner);
        if(status!=PXA_STATUS_OK)continue;
        if(!*epoch || !(volume->capabilities&PXA_VOLUME_REMOVABLE))*epoch=volume->mount_epoch;
        if(!(volume->capabilities&PXA_VOLUME_REMOVABLE))break;
    }
    heap_caps_free(volumes);return *epoch?PXA_STATUS_OK:status;
}
static pxa_status_t idle_volume_home(uint64_t epoch,const char *locator) {
    char mount[PXA_ESP_STORAGE_PATH_BYTES],home[PXA_POSIX_APP_PATH_BYTES];
    pxa_status_t status=pxa_esp_storage_volume_path(epoch,mount,sizeof(mount));
    if(status!=PXA_STATUS_OK)return status;
    int length=snprintf(home,sizeof(home),"%s/PXA/apps/%s",mount,locator);
    if(length<0 || (size_t)length>=sizeof(home))return PXA_STATUS_LIMIT_EXCEEDED;
    status=pxa_esp_volumes_bind_app_home(home,epoch);
    if(status==PXA_STATUS_OK)status=pxa_esp_volumes_unbind_private(home,epoch);
    return status;
}
static pxa_status_t storage_paths_locked(const char *,int,char *,size_t,char *,size_t);
typedef struct {
    char private_root[512],media_relative[160],mount[PXA_ESP_STORAGE_PATH_BYTES];
    char media_mount[PXA_ESP_STORAGE_PATH_BYTES],absolute[512];
    pxa_volume_info_t volume;
} legacy_paths_t;
static pxa_status_t legacy_media_at(legacy_paths_t *paths,pxa_esp_legacy_source_t *source) {
    int n=snprintf(paths->absolute,sizeof(paths->absolute),"%s/%s%s%s",CONFIG_PXA_MOUNT_POINT,
        paths->media_mount,paths->media_mount[0]?"/":"",paths->media_relative);
    if(n<0 || (size_t)n>=sizeof(paths->absolute))return PXA_STATUS_LIMIT_EXCEEDED;
    struct stat st;
    if(lstat(paths->absolute,&st))return errno==ENOENT?PXA_STATUS_NOT_FOUND:PXA_STATUS_IO_ERROR;
    if(!S_ISDIR(st.st_mode))return PXA_STATUS_DENIED;
    pxa_status_t status=pxa_esp_storage_resolve_volume(paths->absolute,&paths->volume);
    if(status==PXA_STATUS_OK) {
        source->media_epoch=paths->volume.mount_epoch;
        snprintf(source->media,sizeof(source->media),"%s",paths->media_relative);
    }
    return status;
}
static pxa_status_t legacy_media_source(legacy_paths_t *paths,pxa_esp_legacy_source_t *source,
    const uint8_t publisher[32],const char *app_id) {
    pxa_status_t status=pxa_esp_storage_media_mount(paths->media_mount,sizeof(paths->media_mount));
    if(status==PXA_STATUS_UNAVAILABLE || status==PXA_STATUS_NOT_FOUND)return PXA_STATUS_OK;
    if(status!=PXA_STATUS_OK)return status;
    status=legacy_media_at(paths,source);
    if(status!=PXA_STATUS_NOT_FOUND)return status;
    /* Only the explicit upgrade recognizes an EXISTING old AppKey directory.
     * Normal runtime paths remain publisher/App roots and never create this
     * obsolete alternate namespace. Missing media is not manufactured. */
    static const char domain[]="PXA-APPKEY-1",hex[]="0123456789abcdef";
    uint8_t input[sizeof(domain)-1+34+64],hash[32];size_t n=sizeof(domain)-1,len=strlen(app_id);
    memcpy(input,domain,n);memcpy(input+n,publisher,32);input[n+32]=(uint8_t)len;input[n+33]=0;
    memcpy(input+n+34,app_id,len);
    status=pxa_esp_mbedtls_sha256(input,n+34+len,hash);if(status!=PXA_STATUS_OK)return status;
    memcpy(paths->media_relative,"PXA/media/",10);
    for(unsigned i=0;i<32;++i) {paths->media_relative[10+2*i]=hex[hash[i]>>4];paths->media_relative[11+2*i]=hex[hash[i]&15];}
    paths->media_relative[74]=0;
    status=legacy_media_at(paths,source);return status==PXA_STATUS_NOT_FOUND?PXA_STATUS_OK:status;
}
/* Metadata lock held. Snapshot only existing catalog/legacy adapters; this
 * does not reserve a new namespace or persist any NVS locator. */
static pxa_status_t legacy_source_locked(const pxa_esp_package_entry_t *entry,pxa_esp_legacy_source_t *source) {
    if(!entry || !entry->installed || entry->volume_local || entry->built_in || entry->ambiguous || !entry->has_publisher_key_id)
        return PXA_STATUS_BAD_STATE;
    memset(source,0,sizeof(*source));
    legacy_paths_t *paths=esp_alloc(sizeof(*paths));if(!paths)return PXA_STATUS_RESOURCE_LIMIT;
    uint16_t flags=0;char relative[130];
    pxa_status_t status=pxa_app_locators_find(&g_store->locators,entry->publisher_key_id,entry->id,&flags,relative,sizeof(relative));
    if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;
    if(status==PXA_STATUS_OK && (flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING)))status=PXA_STATUS_BUSY;
    if(status==PXA_STATUS_OK)status=storage_paths_locked(entry->identity,1,paths->private_root,sizeof(paths->private_root),
        paths->media_relative,sizeof(paths->media_relative));
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_resolve_volume(entry->root,&paths->volume);
    if(status==PXA_STATUS_OK && !(paths->volume.capabilities&PXA_VOLUME_PRIVATE_NATIVE))status=PXA_STATUS_UNSUPPORTED;
    if(status==PXA_STATUS_OK) {
        source->app_epoch=paths->volume.mount_epoch;
        status=pxa_esp_storage_volume_path(source->app_epoch,paths->mount,sizeof(paths->mount));
    }
    if(status==PXA_STATUS_OK) {
        size_t prefix=strlen(paths->mount);
        if(strncmp(entry->root,paths->mount,prefix) || entry->root[prefix]!='/' ||
            strncmp(paths->private_root,paths->mount,prefix) || paths->private_root[prefix]!='/')status=PXA_STATUS_DENIED;
        else if(strlen(entry->root+prefix+1)>=sizeof(source->package) ||
            strlen(paths->private_root+prefix+1)>=sizeof(source->private_root))status=PXA_STATUS_LIMIT_EXCEEDED;
        else {strcpy(source->package,entry->root+prefix+1);strcpy(source->private_root,paths->private_root+prefix+1);}
    }
    if(status==PXA_STATUS_OK)status=legacy_media_source(paths,source,entry->publisher_key_id,entry->id);
    heap_caps_free(paths);return status;
}
static pxa_status_t prepare_move_kind(const char *identity,pxa_esp_move_approval_t *approval,uint8_t resources_only) {
    if(!approval || !identity || !store_is_initialized())return PXA_STATUS_INVALID_ARGUMENT;
    memset(approval,0,sizeof(*approval));
    approval->resources_only=resources_only;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_BUSY;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return PXA_STATUS_BUSY;}
    pxa_esp_package_entry_t *entry=find_entry(identity);pxa_status_t status=PXA_STATUS_BAD_STATE;
    if(!entry || !package_entry_has_home(entry) ||
        entry->built_in || entry->ambiguous)goto metadata_done;
    if(resources_only && entry->archived) {status=PXA_STATUS_UNSUPPORTED;goto metadata_done;}
    if(!entry->volume_local) {
        if(resources_only) {status=PXA_STATUS_UNSUPPORTED;goto metadata_done;}
        status=legacy_source_locked(entry,&approval->legacy);
        if(status!=PXA_STATUS_OK)goto metadata_done;
        approval->legacy_upgrade=1;approval->source_epoch=approval->legacy.app_epoch;
        approval->source.flags=entry->enabled?PXA_INSTALL_RECORD_ENABLED:0;
        snprintf(approval->identity,sizeof(approval->identity),"%s",entry->identity);
        snprintf(approval->name,sizeof(approval->name),"%.*s",(int)sizeof(approval->name)-1,entry->name);
        snprintf(approval->version,sizeof(approval->version),"%s",entry->version);
        goto metadata_done;
    }
    approval->source_epoch=entry->mount_epoch;
    approval->source.generation=entry->install_generation;
    snprintf(approval->identity,sizeof(approval->identity),"%s",entry->identity);
    snprintf(approval->locator,sizeof(approval->locator),"%s",entry->storage_key);
    snprintf(approval->name,sizeof(approval->name),"%.*s",(int)sizeof(approval->name)-1,entry->name);
    snprintf(approval->version,sizeof(approval->version),"%s",entry->version);
    status=PXA_STATUS_OK;
metadata_done:
    release_lock(g_store->metadata_lock);
    if(status!=PXA_STATUS_OK)goto done;
    if(approval->legacy_upgrade) {
        status=pxa_esp_legacy_adoption_prepare(&approval->legacy,!!(approval->source.flags&PXA_INSTALL_RECORD_ENABLED),
            &approval->source,&approval->choices[0].estimate);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_locator(approval->source.publisher_root,approval->source.app_id,
            approval->locator,sizeof(approval->locator));
        if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume(approval->source_epoch,&approval->choices[0].volume);
        if(status==PXA_STATUS_OK) {
            approval->count=1;
            snprintf(approval->current_volume,sizeof(approval->current_volume),"%s",approval->choices[0].volume.name);
            if(approval->legacy.media_epoch) {
                pxa_volume_info_t media;
                if(pxa_esp_storage_volume(approval->legacy.media_epoch,&media)==PXA_STATUS_OK)
                    snprintf(approval->media_volume,sizeof(approval->media_volume),"%s",media.name);
            }
        }
        goto done;
    }
    status=idle_volume_home(approval->source_epoch,approval->locator);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_refresh_volume(approval->source_epoch);
    if(status!=PXA_STATUS_OK)goto done;
    pxa_volume_info_t *volumes=heap_caps_aligned_calloc(8,PXA_ESP_STORAGE_MAX_VOLUMES,sizeof(*volumes),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    pxa_install_record_t *record=heap_caps_aligned_calloc(8,1,sizeof(*record),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!volumes || !record) {heap_caps_free(volumes);heap_caps_free(record);status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    uint64_t generation=approval->source.generation;pxa_status_t last=PXA_STATUS_UNAVAILABLE;
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        pxa_volume_info_t *volume=&volumes[i];
        if(!resources_only && volume->mount_epoch==approval->source_epoch) {
            snprintf(approval->current_volume,sizeof(approval->current_volume),"%s",volume->name);continue;
        }
        const uint64_t required=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || (volume->capabilities&required)!=required)continue;
        if(pxa_esp_storage_refresh_volume(volume->mount_epoch)!=PXA_STATUS_OK ||
            pxa_esp_storage_volume(volume->mount_epoch,volume)!=PXA_STATUS_OK)continue;
        pxa_esp_install_choice_t *choice=&approval->choices[approval->count];
        uint64_t media_epoch=0;
        if(resources_only)last=pxa_esp_resource_transfer_plan(approval->source_epoch,volume->mount_epoch,
            approval->locator,generation,record,&media_epoch,&choice->estimate);
        else last=pxa_esp_app_transfer_plan(approval->source_epoch,volume->mount_epoch,approval->locator,generation,record,&choice->estimate);
        if(last!=PXA_STATUS_OK)continue;
        approval->source=*record;choice->volume=*volume;
        if(resources_only)approval->source_media_epoch=media_epoch;
        if(++approval->count==PXA_ESP_INSTALL_CHOICES)break;
    }
    if(approval->count && resources_only) {
        for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
            if(volumes[i].mount_epoch==approval->source_media_epoch)
                snprintf(approval->current_volume,sizeof(approval->current_volume),"%s",volumes[i].name);
            if(volumes[i].mount_epoch==approval->source_epoch)
                snprintf(approval->media_volume,sizeof(approval->media_volume),"%s",volumes[i].name);
        }
    } else if(approval->count && (approval->source.flags&PXA_INSTALL_RECORD_HAS_MEDIA))
        for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i)
            if(!memcmp(volumes[i].id.bytes,approval->source.media_volume.bytes,16))
                snprintf(approval->media_volume,sizeof(approval->media_volume),"%s",volumes[i].name);
    heap_caps_free(volumes);heap_caps_free(record);
    status=approval->count?PXA_STATUS_OK:last;
done:
    release_lock(g_store->transaction_lock);return status;
}
pxa_status_t pxa_esp_package_store_prepare_move(const char *identity,pxa_esp_move_approval_t *approval) {
    return prepare_move_kind(identity,approval,0);
}
pxa_status_t pxa_esp_package_store_prepare_move_resources(const char *identity,pxa_esp_move_approval_t *approval) {
    return prepare_move_kind(identity,approval,1);
}
pxa_status_t pxa_esp_package_store_commit_move(const pxa_esp_move_approval_t *approval,size_t choice,
    pxa_esp_transfer_progress_fn progress,void *context) {
    if(!approval || choice>=approval->count || choice>=PXA_ESP_INSTALL_CHOICES || !store_is_initialized())
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_posix_app_home_t *home=heap_caps_aligned_calloc(8,1,sizeof(*home),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!home)return PXA_STATUS_RESOURCE_LIMIT;
    if(!take_lock(g_store->transaction_lock)) {heap_caps_free(home);return PXA_STATUS_BUSY;}
    const uint64_t target=approval->choices[choice].volume.mount_epoch;
    pxa_status_t status=PXA_STATUS_OK;
    if(approval->legacy_upgrade) {
        if(!take_lock(g_store->metadata_lock))status=PXA_STATUS_BUSY;
        else {
            const pxa_esp_package_entry_t *entry=find_entry(approval->identity);
            if(!entry || !entry->installed || entry->volume_local || entry->built_in || entry->ambiguous ||
                entry->enabled!=!!(approval->source.flags&PXA_INSTALL_RECORD_ENABLED))status=PXA_STATUS_BAD_STATE;
            release_lock(g_store->metadata_lock);
        }
    }
    if(status==PXA_STATUS_OK)status=idle_volume_home(approval->source_epoch,approval->locator);
    if(status==PXA_STATUS_OK && !approval->resources_only)status=idle_volume_home(target,approval->locator);
    const pxa_esp_app_transfer_control_t control={context,NULL,progress};
    if(status==PXA_STATUS_OK) {
        if(approval->legacy_upgrade) {
            if(target!=approval->source_epoch)status=PXA_STATUS_BAD_STATE;
            else status=pxa_esp_legacy_adoption_begin(&approval->legacy,&approval->source,&control,home);
        } else if(approval->resources_only)status=pxa_esp_resource_transfer_begin_approved(approval->source_epoch,
            approval->source_media_epoch,target,approval->locator,&approval->source,home);
        else status=pxa_esp_app_transfer_begin_approved(approval->source_epoch,target,
            approval->locator,&approval->source,&control,NULL,home);
    }
    if(take_lock(g_store->metadata_lock)) {
        if(status==PXA_STATUS_OK || (approval->resources_only && home->record.generation)) {
            pxa_esp_package_entry_t *entry=find_entry(approval->identity);
            if(entry) {entry->installed=0;entry->volume_local=0;entry->ambiguous=0;}
            volume_catalog_context_t scan={approval->resources_only?approval->source_epoch:target,0};
            if((!catalog_volume_home(&scan,home) || scan.rejected) && status==PXA_STATUS_OK)status=PXA_STATUS_RESOURCE_LIMIT;
        }
        release_lock(g_store->metadata_lock);
    } else if(status==PXA_STATUS_OK)status=PXA_STATUS_BUSY;
    release_lock(g_store->transaction_lock);heap_caps_free(home);return status;
}
typedef struct {
    pxa_posix_app_home_t home;
    char locator[PXA_POSIX_APP_LOCATOR_BYTES];
} volume_move_t;
pxa_status_t pxa_esp_package_store_move_volume_with_progress(const char *identity,uint64_t target_epoch,uint8_t resume,
    pxa_esp_transfer_progress_fn progress,void *context) {
    if(!store_is_initialized() || !identity || !target_epoch || resume>1 || strlen(identity)<66 ||
        strlen(identity)>=PXA_ESP_PACKAGE_ID_BYTES || identity[64]!=':')return PXA_STATUS_INVALID_ARGUMENT;
    volume_move_t *move=heap_caps_aligned_calloc(8,1,sizeof(*move),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_status_t status=PXA_STATUS_UNAVAILABLE;uint64_t source_epoch=0,generation=0;
    const pxa_esp_app_transfer_control_t control={context,NULL,progress};
    const char *stage="lock";
    if(!take_lock(g_store->transaction_lock))goto free_move;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);goto free_move;}
    pxa_esp_package_entry_t *entry=find_entry(identity);
    if(!resume && (!entry || !package_entry_has_home(entry) || !entry->volume_local || entry->ambiguous || entry->built_in)) {
        status=entry && !entry->volume_local?PXA_STATUS_UNSUPPORTED:PXA_STATUS_BAD_STATE;
        release_lock(g_store->metadata_lock);goto done;
    }
    strcpy(move->locator,identity);move->locator[64]='/';
    if(entry) {source_epoch=entry->mount_epoch;generation=entry->install_generation;}
    release_lock(g_store->metadata_lock);
    stage="idle-home";status=idle_volume_home(target_epoch,move->locator);
    if(status==PXA_STATUS_OK && !resume)status=idle_volume_home(source_epoch,move->locator);
    if(status!=PXA_STATUS_OK)goto done;
    stage="transfer";
    if(resume) {
        status=pxa_esp_legacy_adoption_resume(target_epoch,move->locator,&control,&move->home);
        if(status==PXA_STATUS_UNSUPPORTED)status=pxa_esp_app_transfer_resume(target_epoch,move->locator,&control,NULL,&move->home);
    } else status=pxa_esp_app_transfer_begin(source_epoch,target_epoch,move->locator,generation,&control,NULL,&move->home);
    if(take_lock(g_store->metadata_lock)) {
        entry=find_entry(identity);
        if(status==PXA_STATUS_OK) {
            if(entry) {entry->installed=0;entry->volume_local=0;entry->ambiguous=0;}
            volume_catalog_context_t scan={target_epoch,0};
            if(!catalog_volume_home(&scan,&move->home) || scan.rejected)status=PXA_STATUS_RESOURCE_LIMIT;
        }
        release_lock(g_store->metadata_lock);
    } else if(status==PXA_STATUS_OK)status=PXA_STATUS_UNAVAILABLE;
done:
    release_lock(g_store->transaction_lock);
free_move:
    if(status!=PXA_STATUS_OK)ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Whole-app move failed at %s: status=%ld",stage,(long)status);
    heap_caps_free(move);return status;
}
pxa_status_t pxa_esp_package_store_move_volume(const char *identity,uint64_t target_epoch,uint8_t resume) {
    return pxa_esp_package_store_move_volume_with_progress(identity,target_epoch,resume,NULL,NULL);
}
typedef struct {uint64_t epoch;size_t completed,pending;} transfer_bootstrap_t;
static int recover_transfer(void *context,const char *locator) {
    transfer_bootstrap_t *scan=context;char identity[PXA_ESP_PACKAGE_ID_BYTES];
    snprintf(identity,sizeof(identity),"%s",locator);identity[64]=':';
    pxa_status_t status=pxa_esp_package_store_move_volume(identity,scan->epoch,1);
    if(status==PXA_STATUS_OK)++scan->completed;
    else {
        ++scan->pending;
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Transfer recovery remains pending: app=%s status=%ld",locator+65,(long)status);
    }
    return 1;
}
pxa_status_t pxa_esp_package_store_recover_transfers(size_t *completed,size_t *pending) {
    if(!completed || !pending)return PXA_STATUS_INVALID_ARGUMENT;
    *completed=*pending=0;
    if(!store_is_initialized())return PXA_STATUS_UNAVAILABLE;
    pxa_volume_info_t *volumes=heap_caps_aligned_calloc(8,PXA_ESP_STORAGE_MAX_VOLUMES,sizeof(*volumes),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!volumes)return PXA_STATUS_RESOURCE_LIMIT;
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);pxa_status_t result=PXA_STATUS_OK;
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        if(volumes[i].state!=PXA_VOLUME_READY || volumes[i].ambiguous)continue;
        transfer_bootstrap_t scan={volumes[i].mount_epoch,0,0};size_t found=0;
        pxa_status_t status=pxa_esp_app_transfer_visit(scan.epoch,recover_transfer,&scan,&found);
        *completed+=scan.completed;*pending+=scan.pending;
        if(status!=PXA_STATUS_OK)result=status;
    }
    heap_caps_free(volumes);
    if(*completed) {
        pxa_esp_package_store_sync_fn callback=NULL;void *context=NULL;
        if(take_lock(g_store->metadata_lock)) {
            callback=g_store->sync_callback;context=g_store->sync_callback_user_data;
            release_lock(g_store->metadata_lock);
        }
        if(callback)callback(context);
    }
    return result!=PXA_STATUS_OK?result:*pending?PXA_STATUS_WOULD_BLOCK:PXA_STATUS_OK;
}
static __attribute__((noinline)) pxa_status_t bind_prepared_media(const pxa_package_manifest_t *manifest,
    uint64_t app_epoch,pxa_install_record_t *record) {
    if(manifest->format_minor<8)return PXA_STATUS_OK;
    if(!manifest->resource_manifest_sha256) {
        if(!(record->flags&PXA_INSTALL_RECORD_MANAGED_MEDIA))return PXA_STATUS_OK;
        record->flags&=~(PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA);
        memset(record->media_set_sha256,0,32);memset(&record->media_volume,0,sizeof(record->media_volume));
        return PXA_STATUS_OK;
    }
    struct {pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];char relative[256],path[512],mount[PXA_ESP_STORAGE_PATH_BYTES];} *scratch=
        heap_caps_calloc(1,sizeof(*scratch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!scratch)return PXA_STATUS_RESOURCE_LIMIT;
    record->flags|=PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
    memcpy(record->media_set_sha256,manifest->resource_manifest_sha256,32);
    pxa_status_t status=pxa_esp_installations_media_path(record,scratch->relative,sizeof(scratch->relative));
    if(status!=PXA_STATUS_OK)goto done;
    size_t count=pxa_esp_storage_volumes(scratch->volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    status=PXA_STATUS_UNAVAILABLE;
    for(unsigned pass=0;pass<3 && status!=PXA_STATUS_OK;++pass)for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        const pxa_volume_info_t *volume=&scratch->volumes[i];struct stat info;
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || !(volume->capabilities&PXA_VOLUME_READ))continue;
        if(pass==0 && memcmp(volume->id.bytes,record->media_volume.bytes,16))continue;
        if(pass==1 && volume->mount_epoch!=app_epoch)continue;
        if(pxa_esp_storage_volume_path(volume->mount_epoch,scratch->mount,sizeof(scratch->mount))!=PXA_STATUS_OK)continue;
        int n=snprintf(scratch->path,sizeof(scratch->path),"%s/%s",scratch->mount,scratch->relative);
        if(n<0 || (size_t)n>=sizeof(scratch->path) || stat(scratch->path,&info)!=0 || !S_ISDIR(info.st_mode))continue;
        /* Presence is only a placement/reuse hint. The installation's media
         * callback fully checks this exact signed generation before commit. */
        record->media_volume=volume->id;status=PXA_STATUS_OK;break;
    }
done:
    heap_caps_free(scratch);return status;
}
pxa_status_t pxa_esp_package_store_install_file_on_volume(const char *source,uint64_t epoch,
    const uint8_t expected_root[32],const char *expected_app) {
    pxa_esp_installations_t *owner=NULL;pxa_package_manifest_t *manifest=NULL;
    pxa_posix_installer_result_t result={0};pxa_install_record_t approved={0};
    pxa_posix_app_home_t home;pxa_install_plan_t plan={0};pxa_volume_info_t volume;
    char root[PXA_ESP_PACKAGE_MAX_PATH],identity[PXA_ESP_PACKAGE_MAX_IDENTITY],locator[PXA_POSIX_APP_LOCATOR_BYTES];
    uint8_t publisher[32],digest[32];char app_id[65];uint64_t expected=0,source_epoch=0;pxa_status_t status;
    int legacy_update=0,created_entry=0;
    char *resource_result_root=NULL;
    if(!store_is_initialized() || !source || (expected_root && !expected_app))return PXA_STATUS_INVALID_ARGUMENT;
    status=retain_source_volume(source,&source_epoch);if(status!=PXA_STATUS_OK)return status;
    if(!is_regular_file(source)) {pxa_esp_volumes_release(source_epoch);return PXA_STATUS_INVALID_ARGUMENT;}
    if(!take_lock(g_store->transaction_lock)) {pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    result.struct_size=sizeof(result);result.manifest=&manifest;result.root=root;result.root_capacity=sizeof(root);
    status=read_source_into_store(source,&result,1);if(status!=PXA_STATUS_OK)goto done;
    if(!manifest || !manifest->app_id.size || manifest->app_id.size>=sizeof(app_id) ||
        !manifest_publisher_root(manifest,publisher)) {status=PXA_STATUS_DENIED;goto done;}
    memcpy(app_id,manifest->app_id.data,manifest->app_id.size);app_id[manifest->app_id.size]=0;
    if((expected_root && (memcmp(expected_root,publisher,32) || strcmp(expected_app,app_id))) ||
        !canonical_identity(identity,sizeof(identity),publisher,app_id)) {status=PXA_STATUS_DENIED;goto done;}
    status=pxa_esp_mbedtls_sha256(manifest->encoded.data,manifest->encoded.size,digest);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_locator(publisher,app_id,locator,sizeof(locator));if(status!=PXA_STATUS_OK)goto done;
    if(manifest->package_kind==PXA_PACKAGE_KIND_RESOURCES) {
        resource_result_root=heap_caps_malloc(PXA_POSIX_APP_PATH_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!resource_result_root) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
        result.root=resource_result_root;result.root_capacity=PXA_POSIX_APP_PATH_BYTES;
        if(!epoch) {status=default_install_volume(manifest,&epoch);if(status!=PXA_STATUS_OK)goto done;}
        status=pxa_esp_storage_volume(epoch,&volume);if(status!=PXA_STATUS_OK)goto done;
        approved.flags=PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
        approved.media_volume=volume.id;memcpy(approved.publisher_root,publisher,32);
        memcpy(approved.media_set_sha256,digest,32);strcpy(approved.app_id,app_id);
        status=pxa_esp_installations_open(epoch,&owner);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_plan(owner,manifest,0,0,&plan);
        if(status==PXA_STATUS_OK)status=pxa_esp_storage_reserve_plan(&plan);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_install_resources(owner,&approved,source,&plan,&result);
        goto done;
    }
    pxa_esp_package_entry_t *entry=find_entry(identity);
    if(!epoch && entry && entry->installed && !entry->volume_local && !entry->ambiguous) {legacy_update=1;goto done;}
    if(!epoch && entry && entry->volume_local)epoch=entry->mount_epoch;
    if(!epoch) {status=default_install_volume(manifest,&epoch);if(status!=PXA_STATUS_OK)goto done;}
    status=installation_path_fits(epoch,locator);if(status!=PXA_STATUS_OK)goto done;
    if(entry && (entry->installed || entry->volume_local) && (!entry->volume_local || entry->mount_epoch!=epoch || entry->ambiguous)) {
        status=PXA_STATUS_BAD_STATE;goto done; /* Explicit migration/instance resolution, never a second save. */
    }
    if(!entry) {entry=reserve_entry(app_id,publisher);created_entry=entry!=NULL;}
    if(!entry) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=pxa_esp_storage_volume(epoch,&volume);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_open(epoch,&owner);if(status!=PXA_STATUS_OK)goto done;
    if(entry->installed || entry->archived) {
        /* The transaction lock excludes new activation while this probe is
         * released. Existing activation pins the home and rejects replacement
         * before any executable slot is renamed. */
        status=idle_volume_home(epoch,locator);if(status!=PXA_STATUS_OK)goto done;
        if(!bind_store_result_buffers(&result)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
        status=pxa_esp_installations_load(owner,locator,NULL,NULL,&result,&home);if(status!=PXA_STATUS_OK)goto done;
        approved=home.record;expected=approved.generation;approved.flags&=~PXA_INSTALL_RECORD_ARCHIVED;
        /* Reload the exact candidate after recovery reused result buffers. */
        status=read_source_into_store(source,&result,1);if(status!=PXA_STATUS_OK)goto done;
    } else {
        approved.flags=PXA_INSTALL_RECORD_ENABLED;approved.app_volume=volume.id;
        approved.data_generation=1;
        memcpy(approved.publisher_root,publisher,32);strcpy(approved.app_id,app_id);
        do {esp_fill_random(approved.install_id,16);} while(!memcmp(approved.install_id,(uint8_t[16]){0},16));
    }
    status=bind_prepared_media(manifest,epoch,&approved);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_plan(owner,manifest,0,!expected,&plan);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_storage_reserve_plan(&plan);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_install(owner,source,locator,&approved,expected,digest,&plan,NULL,NULL,&result,&home);
    if(status==PXA_STATUS_OK) {
        volume_catalog_context_t scan={epoch,0};
        if(!catalog_volume_home(&scan,&home) || scan.rejected)status=PXA_STATUS_RESOURCE_LIMIT;
    }
done:
    if(plan.reserved) {
        (void)pxa_esp_storage_release_plan(&plan);
        (void)pxa_esp_storage_refresh_volume(epoch);
    }
    status=close_volume_operation(&owner,status);
    heap_caps_free(resource_result_root);
    if(status!=PXA_STATUS_OK && created_entry) {
        pxa_esp_package_entry_t *failed=find_entry(identity);
        if(failed && !failed->installed && !failed->staged)remove_entry_at((size_t)(failed-g_store->entries));
    }
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);
    if(legacy_update)status=install_file_for_owner(source,expected_root,expected_app)?PXA_STATUS_OK:PXA_STATUS_INTERNAL;
    pxa_esp_volumes_release(source_epoch);return status;
}

pxa_status_t pxa_esp_package_store_open_storage(const char *identity,pxa_esp_app_storage_t **storage,
    uint64_t *media_epoch,char *media_relative,size_t media_capacity,pxa_posix_installer_result_t *verification) {
    pxa_status_t status=PXA_STATUS_NOT_FOUND;pxa_esp_installations_t *owner=NULL;
    pxa_posix_app_home_t home;
    char locator[PXA_POSIX_APP_LOCATOR_BYTES];
    uint8_t install_id[16],manifest_hash[32];uint64_t epoch,generation;
    if(!store_is_initialized() || !identity || !storage || !media_epoch || !media_relative || !media_capacity || !verification)
        return PXA_STATUS_INVALID_ARGUMENT;
    *storage=NULL;*media_epoch=0;media_relative[0]=0;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    pxa_esp_package_entry_t *entry=find_entry(identity);
    if(!entry || !entry->installed || entry->ambiguous || !entry->volume_local) {
        status=!entry || !entry->installed?PXA_STATUS_NOT_FOUND:
            entry->ambiguous?PXA_STATUS_BAD_STATE:PXA_STATUS_OK;
        release_lock(g_store->metadata_lock);goto done;
    }
    epoch=entry->mount_epoch;generation=entry->install_generation;
    memcpy(install_id,entry->install_id,16);memcpy(manifest_hash,entry->manifest_sha256,32);
    snprintf(locator,sizeof(locator),"%s",entry->storage_key);
    /* Bulk verification uses the activation's own PSRAM buffers. Catalog
     * readers never wait for payload hashing or a private-image mount. */
    release_lock(g_store->metadata_lock);
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_open(epoch,&owner);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_load(owner,locator,NULL,NULL,verification,&home);if(status!=PXA_STATUS_OK)goto done;
    if(memcmp(home.record.install_id,install_id,16) || home.record.generation<generation ||
        memcmp(home.record.manifest_sha256,manifest_hash,32)) {
        status=PXA_STATUS_BAD_STATE;goto done;
    }
    /* The authenticated disk record is authoritative after an uncertain
     * policy/data commit. Heal a stale cache without accepting other code. */
    if(!take_lock(g_store->metadata_lock)) {status=PXA_STATUS_UNAVAILABLE;goto done;}
    entry=find_entry(identity);
    if(!entry || !entry->volume_local || entry->mount_epoch!=epoch || memcmp(entry->install_id,install_id,16)) {
        release_lock(g_store->metadata_lock);status=PXA_STATUS_BAD_STATE;goto done;
    }
    entry->install_generation=home.record.generation;entry->data_generation=home.record.data_generation;
    entry->enabled=!!(home.record.flags&PXA_INSTALL_RECORD_ENABLED) && !(home.record.flags&PXA_INSTALL_RECORD_ARCHIVED);
    release_lock(g_store->metadata_lock);
    if(!(home.record.flags&PXA_INSTALL_RECORD_ENABLED) || (home.record.flags&PXA_INSTALL_RECORD_ARCHIVED)) {
        status=PXA_STATUS_DENIED;goto done;
    }
    uint64_t selected_media_epoch=epoch;
    if(home.record.flags&PXA_INSTALL_RECORD_HAS_MEDIA) {
        pxa_volume_info_t media_volume;
        status=pxa_esp_storage_find_volume(&home.record.media_volume,&media_volume);
        if(status!=PXA_STATUS_OK)goto done;
        selected_media_epoch=media_volume.mount_epoch;
        status=pxa_esp_installations_media_path(&home.record,media_relative,media_capacity);
        if(status!=PXA_STATUS_OK)goto done;
    } else {
        int n=snprintf(media_relative,media_capacity,"PXA/media/%s",home.locator);
        if(n<0 || (size_t)n>=media_capacity) {status=PXA_STATUS_LIMIT_EXCEEDED;goto done;}
    }
    status=pxa_esp_installations_storage(owner,&home,storage);
    if(status==PXA_STATUS_OK)*media_epoch=selected_media_epoch;
done:
    status=close_volume_operation(&owner,status);
    release_lock(g_store->transaction_lock);return status;
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
    if (!store_is_initialized() || !g_store->legacy_ready || !safe_app_id(app_id) ||
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
    if (!store_is_initialized() || !g_store->legacy_ready || !take_lock(g_store->transaction_lock)) return;
    (void)pxa_posix_installer_discard_transfers(g_store->installer);
    release_lock(g_store->transaction_lock);
}

pxa_status_t pxa_esp_package_store_try_discard_transfers(void) {
    if (!store_is_initialized() || !g_store->legacy_ready) return PXA_STATUS_OK;
    SemaphoreHandle_t lock=g_store->transaction_lock;
    if (!lock || xSemaphoreGetMutexHolder(lock)==xTaskGetCurrentTaskHandle() ||
        xSemaphoreTake(lock,0)!=pdTRUE) return PXA_STATUS_WOULD_BLOCK;
    pxa_status_t status=pxa_posix_installer_discard_transfers(g_store->installer);
    release_lock(lock);
    return status;
}

static int preview_manifest(const pxa_package_manifest_t *manifest,pxa_esp_package_preview_t *preview) {
    uint8_t publisher_root[32];
    memset(preview,0,sizeof(*preview));
    copy_bytes_text(preview->app_id, sizeof(preview->app_id), manifest->app_id);
    if (!manifest_publisher_root(manifest, publisher_root) ||
        !canonical_identity(preview->id, sizeof(preview->id), publisher_root,
                            preview->app_id)) {
        return 0;
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
        return 0;
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
        if (target->name[0] == '\0') return 0;
        target->required = permission->required != 0;
        ++preview->permission_count;
    }
    return 1;
}

pxa_status_t pxa_esp_package_store_prepare_install(const uint8_t *prefix,size_t bytes,uint64_t total,uint64_t transfer,uint8_t download_only,
    const uint8_t expected_root[32],const char *expected_app,
    pxa_esp_package_preview_t *preview,pxa_esp_install_approval_t *approval) {
    pxa_container_header_t header;pxa_package_limits_t limits;size_t workspace;
    pxa_package_manifest_t *manifest=NULL;pxa_posix_installer_result_t result={0};
    char metadata_root[1];
    pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home;
    pxa_status_t status;uint64_t existing_epoch=0;
    if(!store_is_initialized() || !prefix || !preview || !approval || (expected_root && !expected_app) ||
        bytes<PXA_CONTAINER_HEADER_BYTES)return PXA_STATUS_INVALID_ARGUMENT;
    memset(approval,0,sizeof(*approval));memset(preview,0,sizeof(*preview));
    approval->download_only=!!download_only;
    status=pxa_container_header_parse((pxa_bytes_t){prefix,PXA_CONTAINER_HEADER_BYTES},total,&header);
    if(status!=PXA_STATUS_OK)return status;
    if(header.payload_offset!=bytes)return PXA_STATUS_DENIED;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    if(!manifest_workspace_for_encoded(prefix+header.manifest_offset,header.manifest_size,&limits,&workspace) ||
        !ensure_encoded_capacity(header.manifest_size) || !ensure_manifest_workspace_capacity(workspace)) {
        status=PXA_STATUS_RESOURCE_LIMIT;goto done;
    }
    result.struct_size=sizeof(result);result.manifest=&manifest;result.root=metadata_root;result.root_capacity=sizeof(metadata_root);
    if(!bind_store_result_buffers(&result)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=pxa_posix_installer_read_install_metadata(g_store->installer,(pxa_bytes_t){prefix,bytes},total,&result,&header);
    if(status!=PXA_STATUS_OK)goto done;
    if(manifest->package_kind!=PXA_PACKAGE_KIND_APPLICATION) {status=PXA_STATUS_UNSUPPORTED;goto done;}
    if(!preview_manifest(manifest,preview) || (expected_app && strcmp(preview->app_id,expected_app)) ||
        (expected_root && memcmp(expected_root,preview->publisher_root,32))) {status=PXA_STATUS_DENIED;goto done;}
    strcpy(approval->identity,preview->id);
    status=pxa_esp_installations_locator(preview->publisher_root,preview->app_id,approval->locator,sizeof(approval->locator));
    if(status==PXA_STATUS_OK)status=pxa_esp_mbedtls_sha256(manifest->encoded.data,manifest->encoded.size,approval->manifest_sha256);
    if(status!=PXA_STATUS_OK)goto done;
    pxa_esp_package_entry_t *entry=find_entry(preview->id);
    if(!download_only && entry && (entry->installed || entry->volume_local)) {
        if(entry->ambiguous) {status=PXA_STATUS_BAD_STATE;goto done;}
        /* Legacy data must be explicitly migrated before a whole-app update;
         * never create an apparently fresh installation on another volume. */
        if(!entry->volume_local) {status=PXA_STATUS_UNSUPPORTED;goto done;}
        existing_epoch=entry->mount_epoch;
        status=pxa_esp_installations_open(existing_epoch,&owner);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_lookup(owner,approval->locator,&home);
        if(status==PXA_STATUS_OK) {
            approval->record=home.record;approval->expected_generation=home.record.generation;
            approval->record.flags&=~PXA_INSTALL_RECORD_ARCHIVED;
        }
        status=close_volume_operation(&owner,status);if(status!=PXA_STATUS_OK)goto done;
    } else {
        approval->record.flags=PXA_INSTALL_RECORD_ENABLED;approval->record.data_generation=1;
        memcpy(approval->record.publisher_root,preview->publisher_root,32);
        strcpy(approval->record.app_id,preview->app_id);
        do {esp_fill_random(approval->record.install_id,16);} while(!memcmp(approval->record.install_id,(uint8_t[16]){0},16));
    }
    if(manifest->format_minor>=8) {
        if(manifest->resource_manifest_sha256) {
            approval->requires_resources=1;
            approval->record.flags|=PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
            memcpy(approval->record.media_set_sha256,manifest->resource_manifest_sha256,32);
        } else if(approval->record.flags&PXA_INSTALL_RECORD_MANAGED_MEDIA) {
            approval->record.flags&=~(PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA);
            memset(&approval->record.media_volume,0,sizeof(approval->record.media_volume));
            memset(approval->record.media_set_sha256,0,32);
        }
    }
    pxa_volume_info_t *volumes=esp_alloc(sizeof(*volumes)*PXA_ESP_STORAGE_MAX_VOLUMES);
    if(!volumes) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    status=PXA_STATUS_QUOTA_EXCEEDED;
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        const uint64_t required=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
        pxa_volume_info_t *volume=&volumes[i];
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || (volume->capabilities&required)!=required ||
            (existing_epoch && volume->mount_epoch!=existing_epoch))continue;
        if(!download_only && installation_path_fits(volume->mount_epoch,approval->locator)!=PXA_STATUS_OK)continue;
        pxa_status_t candidate;
        pxa_esp_install_choice_t *choice=&approval->choices[approval->count];
        candidate=pxa_install_file_allocation(volume,transfer,&choice->temporary_bytes);
        if(candidate!=PXA_STATUS_OK)continue;
        if(download_only) {
            pxa_install_requirements_t requirements={.app_bytes=UINT64_C(5)*volume->allocation_unit,.volume_headroom_bytes=4096};
            candidate=pxa_install_file_allocation(volume,transfer,&requirements.app_temporary_bytes);
            if(candidate==PXA_STATUS_OK)candidate=pxa_esp_storage_plan(&requirements,volume->mount_epoch,0,&choice->estimate);
        } else {
            candidate=pxa_esp_installations_open(volume->mount_epoch,&owner);
            if(candidate==PXA_STATUS_OK)candidate=pxa_esp_installations_plan(owner,manifest,transfer,!existing_epoch,&choice->estimate);
        }
        candidate=close_volume_operation(&owner,candidate);
        if(candidate!=PXA_STATUS_OK)continue;
        (void)pxa_esp_storage_volume(volume->mount_epoch,&choice->volume);
        if(!approval->count || ((approval->choices[approval->selected].volume.capabilities&PXA_VOLUME_REMOVABLE) &&
            !(volume->capabilities&PXA_VOLUME_REMOVABLE)))approval->selected=approval->count;
        ++approval->count;
    }
    heap_caps_free(volumes);if(approval->count)status=PXA_STATUS_OK;
done:
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);
    if(status!=PXA_STATUS_OK) {memset(approval,0,sizeof(*approval));memset(preview,0,sizeof(*preview));}
    return status;
}

pxa_status_t pxa_esp_package_store_reserve_install(pxa_esp_install_approval_t *approval,size_t index) {
    if(!approval || index>=approval->count || approval->plan.reserved)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_package_store_evaluate_install(approval,index,approval->media_selected,&approval->plan);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_reserve_plan(&approval->plan);
    if(status==PXA_STATUS_OK) {
        approval->selected=index;approval->record.app_volume=approval->choices[index].volume.id;
        if(approval->requires_resources)approval->record.media_volume=approval->media_choices[approval->media_selected].volume.id;
    }
    return status;
}


pxa_status_t pxa_esp_package_store_prepare_resources(pxa_esp_install_approval_t *approval,
    const uint8_t *prefix,size_t bytes,uint64_t total,uint64_t transfer) {
    if(!store_is_initialized() || !approval || !approval->requires_resources || approval->plan.reserved ||
        (!prefix && (bytes || total || transfer)))return PXA_STATUS_INVALID_ARGUMENT;
    if(approval->download_only)return PXA_STATUS_UNSUPPORTED; /* Resource cache bundles have a separate capability. */
    pxa_package_manifest_t *manifest=NULL;pxa_package_limits_t limits;pxa_container_header_t header;
    pxa_posix_installer_result_t result={0};char empty_root[1];size_t workspace;
    pxa_esp_installations_t *owner=NULL;pxa_status_t status;
    struct {pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];char relative[256],path[512],mount[PXA_ESP_STORAGE_PATH_BYTES];} *scratch=NULL;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    approval->media_count=0;memset(approval->media_fit,0,sizeof(approval->media_fit));
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    if(prefix) {
        status=pxa_container_header_parse((pxa_bytes_t){prefix,bytes<PXA_CONTAINER_HEADER_BYTES?bytes:PXA_CONTAINER_HEADER_BYTES},total,&header);
        if(status!=PXA_STATUS_OK || header.payload_offset!=bytes) {status=PXA_STATUS_DENIED;goto done;}
        if(!manifest_workspace_for_encoded(prefix+header.manifest_offset,header.manifest_size,&limits,&workspace) ||
            !ensure_encoded_capacity(header.manifest_size) || !ensure_manifest_workspace_capacity(workspace)) {
            status=PXA_STATUS_RESOURCE_LIMIT;goto done;
        }
        result.struct_size=sizeof(result);result.manifest=&manifest;result.root=empty_root;result.root_capacity=sizeof(empty_root);
        if(!bind_store_result_buffers(&result)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
        status=pxa_posix_installer_read_install_metadata(g_store->installer,(pxa_bytes_t){prefix,bytes},total,&result,&header);
        if(status!=PXA_STATUS_OK)goto done;
        uint8_t digest[32];
        status=pxa_esp_mbedtls_sha256(manifest->encoded.data,manifest->encoded.size,digest);
        if(status!=PXA_STATUS_OK)goto done;
        if(manifest->package_kind!=PXA_PACKAGE_KIND_RESOURCES || memcmp(digest,approval->record.media_set_sha256,32) ||
            memcmp(manifest->management_key_id,approval->record.publisher_root,32) ||
            manifest->app_id.size!=strlen(approval->record.app_id) ||
            memcmp(manifest->app_id.data,approval->record.app_id,manifest->app_id.size)) {status=PXA_STATUS_DENIED;goto done;}
        approval->resource_container_bytes=total;
    }
    scratch=heap_caps_calloc(1,sizeof(*scratch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!scratch) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=pxa_esp_installations_media_path(&approval->record,scratch->relative,sizeof(scratch->relative));
    if(status!=PXA_STATUS_OK)goto done;
    size_t count=pxa_esp_storage_volumes(scratch->volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        pxa_volume_info_t *volume=&scratch->volumes[i];struct stat info;
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || !(volume->capabilities&PXA_VOLUME_READ))continue;
        if(pxa_esp_storage_volume_path(volume->mount_epoch,scratch->mount,sizeof(scratch->mount))!=PXA_STATUS_OK)continue;
        int n=snprintf(scratch->path,sizeof(scratch->path),"%s/%s",scratch->mount,scratch->relative);
        if(n<0 || (size_t)n>=sizeof(scratch->path))continue;
        pxa_esp_install_choice_t *choice=&approval->media_choices[approval->media_count];memset(choice,0,sizeof(*choice));
        if(stat(scratch->path,&info)==0 && S_ISDIR(info.st_mode))choice->reuse=1;
        else {
            const uint64_t writable=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
            if(!manifest || (volume->capabilities&writable)!=writable)continue;
            status=pxa_esp_installations_open(volume->mount_epoch,&owner);
            if(status==PXA_STATUS_OK)status=pxa_esp_installations_plan(owner,manifest,transfer,0,&choice->estimate);
            status=close_volume_operation(&owner,status);if(status!=PXA_STATUS_OK)continue;
            status=pxa_install_file_allocation(volume,transfer,&choice->temporary_bytes);if(status!=PXA_STATUS_OK)continue;
        }
        if(pxa_esp_storage_volume(volume->mount_epoch,&choice->volume)!=PXA_STATUS_OK)continue;
        ++approval->media_count;
    }
    status=PXA_STATUS_QUOTA_EXCEEDED;
    for(size_t app=0;app<approval->count;++app)for(size_t media=0;media<approval->media_count;++media) {
        pxa_install_plan_t candidate;
        if(pxa_esp_package_store_evaluate_install(approval,app,media,&candidate)==PXA_STATUS_OK)
            approval->media_fit[app]|=(uint8_t)(1u<<media);
    }
    /* Existing installations retain their explicit resource volume. New
     * installs first seek a whole fitting volume, then a fitting split. */
    for(unsigned pass=0;pass<4 && status!=PXA_STATUS_OK;++pass) {
        for(size_t order=0;order<approval->count && status!=PXA_STATUS_OK;++order) {
            size_t app=order==0?approval->selected:order<=approval->selected?order-1:order;
            if(approval->expected_generation && app!=approval->selected)continue;
            for(size_t media=0;media<approval->media_count;++media) {
                if(!(approval->media_fit[app]&(1u<<media)))continue;
                if(pass==0 && (!approval->expected_generation ||
                    memcmp(approval->media_choices[media].volume.id.bytes,approval->record.media_volume.bytes,16)))continue;
                if((pass==1 || pass==2) && approval->choices[app].volume.mount_epoch!=approval->media_choices[media].volume.mount_epoch)continue;
                if(!approval->expected_generation && pass==1 && (approval->choices[app].volume.capabilities&PXA_VOLUME_REMOVABLE))continue;
                if(!approval->expected_generation && pass==2 && !(approval->choices[app].volume.capabilities&PXA_VOLUME_REMOVABLE))continue;
                approval->selected=app;approval->media_selected=media;status=PXA_STATUS_OK;break;
            }
        }
    }
done:
    heap_caps_free(scratch);release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
}

pxa_status_t pxa_esp_package_store_commit_resources(const char *source,pxa_esp_install_approval_t *approval) {
    if(!store_is_initialized() || !source || !approval || !approval->requires_resources ||
        !approval->plan.reserved || approval->media_selected>=approval->media_count)return PXA_STATUS_INVALID_ARGUMENT;
    if(approval->media_choices[approval->media_selected].reuse)return PXA_STATUS_OK;
    uint64_t source_epoch=0;pxa_status_t status=retain_source_volume(source,&source_epoch);
    if(status!=PXA_STATUS_OK)return status;
    if(!take_lock(g_store->transaction_lock)) {pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    pxa_esp_installations_t *owner=NULL;pxa_package_manifest_t *manifest=NULL;
    char *root=heap_caps_malloc(PXA_POSIX_APP_PATH_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    pxa_posix_installer_result_t result={.struct_size=sizeof(result),.manifest=&manifest,
        .root=root,.root_capacity=PXA_POSIX_APP_PATH_BYTES};
    status=retry_volume_cleanup();
    if(status==PXA_STATUS_OK && !root)status=PXA_STATUS_RESOURCE_LIMIT;
    if(status==PXA_STATUS_OK)status=read_source_into_store(source,&result,1);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(approval->media_choices[approval->media_selected].volume.mount_epoch,&owner);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_install_resources(owner,&approval->record,source,&approval->plan,&result);
    status=close_volume_operation(&owner,status);heap_caps_free(root);
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);pxa_esp_volumes_release(source_epoch);return status;
}

pxa_status_t pxa_esp_package_store_commit_install(const char *source,pxa_esp_install_approval_t *approval) {
    pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t *home=NULL;uint64_t source_epoch=0;
    pxa_package_manifest_t *manifest=NULL;char root[PXA_ESP_PACKAGE_MAX_PATH];
    pxa_posix_installer_result_t result={.struct_size=sizeof(result),.manifest=&manifest,.root=root,.root_capacity=sizeof(root)};
    if(!store_is_initialized() || !source || !approval || !approval->plan.reserved || approval->download_only)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=retain_source_volume(source,&source_epoch);if(status!=PXA_STATUS_OK)return status;
    if(!take_lock(g_store->transaction_lock)) {pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);pxa_esp_volumes_release(source_epoch);return PXA_STATUS_UNAVAILABLE;}
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    if(approval->requires_resources) {
        if(approval->media_selected>=approval->media_count) {status=PXA_STATUS_BAD_STATE;goto done;}
        const pxa_esp_install_choice_t *selected=&approval->media_choices[approval->media_selected];
        pxa_volume_info_t mounted;
        status=pxa_esp_storage_volume(selected->volume.mount_epoch,&mounted);
        if(status!=PXA_STATUS_OK || mounted.state!=PXA_VOLUME_READY || mounted.ambiguous ||
            memcmp(mounted.id.bytes,approval->record.media_volume.bytes,16)) {status=PXA_STATUS_UNAVAILABLE;goto done;}
    }
    pxa_esp_package_entry_t *entry=find_entry(approval->identity);
    if(entry && (entry->installed || entry->volume_local) && (!entry->volume_local || entry->ambiguous ||
        entry->mount_epoch!=approval->plan.app_mount_epoch || !approval->expected_generation ||
        memcmp(entry->install_id,approval->record.install_id,16))) {status=PXA_STATUS_BAD_STATE;goto done;}
    home=heap_caps_calloc(1,sizeof(*home),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!home) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=idle_volume_home(approval->plan.app_mount_epoch,approval->locator);if(status!=PXA_STATUS_OK)goto done;
    if(!bind_store_result_buffers(&result)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=pxa_esp_installations_open(approval->plan.app_mount_epoch,&owner);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_install(owner,source,approval->locator,&approval->record,
        approval->expected_generation,approval->manifest_sha256,&approval->plan,NULL,NULL,&result,home);
    if(status==PXA_STATUS_OK) {
        volume_catalog_context_t scan={approval->plan.app_mount_epoch,0};
        if(!catalog_volume_home(&scan,home) || scan.rejected)status=PXA_STATUS_RESOURCE_LIMIT;
    }
done:
    status=close_volume_operation(&owner,status);heap_caps_free(home);
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);
    pxa_esp_volumes_release(source_epoch);
    if(status==PXA_STATUS_OK && g_store->sync_callback)g_store->sync_callback(g_store->sync_callback_user_data);
    return status;
}


/* Local installation uses the same signed preflight and commit as Store,
 * with a retained physical source instead of a download worker/cache token. */
static int file_source_path_valid(const char *source) {
    if(!source || source[0]!='/')return 0;
    const char *part=source+1;
    for(const char *p=part;;++p)if(!*p || *p=='/') {
        const size_t n=(size_t)(p-part);
        if(!n || (n==1 && part[0]=='.') || (n==2 && part[0]=='.' && part[1]=='.'))return 0;
        if(!*p)return 1;
        part=p+1;
    }
}
static pxa_status_t file_signed_prefix(pxa_esp_file_install_t *file,uint8_t *prefix,size_t *bytes,uint64_t *total) {
    const size_t limit=PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+PXA_CONTAINER_HEADER_BYTES+216u;
    int fd=open(file->source,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat info;pxa_status_t status=PXA_STATUS_DENIED;
    if(fd<0)return errno==ENOENT?PXA_STATUS_NOT_FOUND:PXA_STATUS_IO_ERROR;
    if(fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_nlink>1 || info.st_size<(off_t)PXA_CONTAINER_HEADER_BYTES)goto done;
    *total=(uint64_t)info.st_size;
    size_t have=0,need=PXA_CONTAINER_HEADER_BYTES;
    for(;;) {
        while(have<need) {
            ssize_t got=read(fd,prefix+have,need-have);
            if(got<0 && errno==EINTR)continue;
            if(got<=0) {status=PXA_STATUS_IO_ERROR;goto done;}
            have+=(size_t)got;
        }
        if(need!=PXA_CONTAINER_HEADER_BYTES)break;
        pxa_container_header_t header;
        status=pxa_container_header_parse((pxa_bytes_t){prefix,have},*total,&header);
        if(status!=PXA_STATUS_OK)goto done;
        if(header.manifest_size>PXA_ESP_SIGNED_MANIFEST_MAX_BYTES ||
            header.payload_offset>limit || header.payload_offset<=have) {status=PXA_STATUS_LIMIT_EXCEEDED;goto done;}
        need=(size_t)header.payload_offset;
    }
    *bytes=have;status=PXA_STATUS_OK;
done:
    if(close(fd)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    return status;
}
static pxa_status_t file_resource_choices(pxa_esp_file_install_t *file,const uint8_t *prefix,size_t bytes,uint64_t total) {
    pxa_package_manifest_t *manifest=NULL;pxa_package_limits_t limits;pxa_container_header_t header;
    pxa_posix_installer_result_t result={0};char root[1];size_t workspace;
    pxa_esp_install_approval_t *approval=&file->placement;
    pxa_esp_installations_t *owner=NULL;pxa_volume_info_t *volumes=NULL;
    pxa_status_t status=pxa_container_header_parse((pxa_bytes_t){prefix,PXA_CONTAINER_HEADER_BYTES},total,&header);
    if(status!=PXA_STATUS_OK || header.payload_offset!=bytes)return PXA_STATUS_DENIED;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    status=retry_volume_cleanup();if(status!=PXA_STATUS_OK)goto done;
    if(!ensure_encoded_capacity(header.manifest_size) ||
        !manifest_workspace_for_encoded(prefix+header.manifest_offset,header.manifest_size,&limits,&workspace) ||
        !ensure_manifest_workspace_capacity(workspace)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    result.struct_size=sizeof(result);result.manifest=&manifest;result.root=root;result.root_capacity=sizeof(root);
    if(!bind_store_result_buffers(&result)) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=pxa_posix_installer_read_install_metadata(g_store->installer,(pxa_bytes_t){prefix,bytes},total,&result,&header);
    if(status!=PXA_STATUS_OK)goto done;
    if(manifest->package_kind==PXA_PACKAGE_KIND_APPLICATION) {status=PXA_STATUS_OK;goto done;}
    if(manifest->package_kind!=PXA_PACKAGE_KIND_RESOURCES || !preview_manifest(manifest,&file->preview)) {
        status=PXA_STATUS_UNSUPPORTED;goto done;
    }
    file->resources_only=1;approval->requires_resources=1;
    approval->record.flags=PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
    memcpy(approval->record.publisher_root,file->preview.publisher_root,32);
    strcpy(approval->record.app_id,file->preview.app_id);
    strcpy(approval->identity,file->preview.id);
    status=pxa_esp_mbedtls_sha256(manifest->encoded.data,manifest->encoded.size,approval->record.media_set_sha256);
    if(status!=PXA_STATUS_OK)goto done;
    volumes=esp_alloc(sizeof(*volumes)*PXA_ESP_STORAGE_MAX_VOLUMES);
    if(!volumes) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES && approval->count<PXA_ESP_INSTALL_CHOICES;++i) {
        pxa_volume_info_t *volume=&volumes[i];
        const uint64_t writable=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
        if(volume->state!=PXA_VOLUME_READY || volume->ambiguous || (volume->capabilities&writable)!=writable)continue;
        pxa_esp_install_choice_t *choice=&approval->choices[approval->count];
        status=pxa_esp_installations_open(volume->mount_epoch,&owner);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_plan(owner,manifest,0,0,&choice->estimate);
        status=close_volume_operation(&owner,status);if(status!=PXA_STATUS_OK)continue;
        if(pxa_esp_storage_volume(volume->mount_epoch,&choice->volume)!=PXA_STATUS_OK)continue;
        /* Match the source when it fits: a copied offline resource package
         * normally lives on the user's intended storage already. */
        if(!approval->count || volume->mount_epoch==file->source_epoch)approval->selected=approval->count;
        ++approval->count;
    }
    approval->media_count=approval->count;
    memcpy(approval->media_choices,approval->choices,sizeof(approval->media_choices));
    status=approval->count?PXA_STATUS_OK:PXA_STATUS_QUOTA_EXCEEDED;
done:
    heap_caps_free(volumes);release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
}
void pxa_esp_file_install_close(pxa_esp_file_install_t *file) {
    if(!file)return;
    if(file->placement.plan.reserved)(void)pxa_esp_storage_release_plan(&file->placement.plan);
    if(file->source_epoch) {(void)pxa_esp_volumes_release(file->source_epoch);file->source_epoch=0;}
}
pxa_status_t pxa_esp_file_install_prepare(pxa_esp_file_install_t *file) {
    if(!store_is_initialized() || !file || file->source_epoch || file->placement.plan.reserved ||
        strnlen(file->source,sizeof(file->source))>=sizeof(file->source) || !file_source_path_valid(file->source))return PXA_STATUS_INVALID_ARGUMENT;
    memset(&file->placement,0,sizeof(file->placement));memset(&file->preview,0,sizeof(file->preview));file->resources_only=0;
    pxa_status_t status=retain_source_volume(file->source,&file->source_epoch);
    uint8_t *prefix=NULL;size_t bytes=0;uint64_t total=0;
    if(status!=PXA_STATUS_OK)goto done;
    prefix=heap_caps_malloc(PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+PXA_CONTAINER_HEADER_BYTES+216u,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!prefix) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    status=file_signed_prefix(file,prefix,&bytes,&total);
    if(status==PXA_STATUS_OK)status=file_resource_choices(file,prefix,bytes,total);
    if(status==PXA_STATUS_OK && !file->resources_only) {
        status=pxa_esp_package_store_prepare_install(prefix,bytes,total,0,0,NULL,NULL,&file->preview,&file->placement);
        if(status==PXA_STATUS_OK && file->placement.requires_resources)
            status=pxa_esp_package_store_prepare_resources(&file->placement,NULL,0,0,0);
    }
done:
    heap_caps_free(prefix);
    if(status!=PXA_STATUS_OK)pxa_esp_file_install_close(file);
    return status;
}
pxa_status_t pxa_esp_file_install_commit(pxa_esp_file_install_t *file,size_t app,size_t media) {
    if(!file || !file->source_epoch || file->placement.plan.reserved || app>=file->placement.count)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_volume_info_t source;
    pxa_status_t status=pxa_esp_storage_resolve_volume(file->source,&source);
    if(status!=PXA_STATUS_OK || source.mount_epoch!=file->source_epoch || source.state!=PXA_VOLUME_READY || source.ambiguous)
        return PXA_STATUS_UNAVAILABLE;
    pxa_esp_install_approval_t *approval=&file->placement;
    if(file->resources_only) {
        if(media)return PXA_STATUS_INVALID_ARGUMENT;
        approval->plan=approval->choices[app].estimate;
        status=pxa_esp_storage_reserve_plan(&approval->plan);
        if(status==PXA_STATUS_OK) {
            approval->selected=app;approval->media_selected=app;
            approval->record.media_volume=approval->choices[app].volume.id;
            status=pxa_esp_package_store_commit_resources(file->source,approval);
        }
    } else {
        if(approval->requires_resources) {
            if(media>=approval->media_count || !(approval->media_fit[app]&(1u<<media)))return PXA_STATUS_INVALID_ARGUMENT;
        } else if(media)return PXA_STATUS_INVALID_ARGUMENT;
        approval->media_selected=media;
        status=pxa_esp_package_store_reserve_install(approval,app);
        if(status==PXA_STATUS_OK)status=pxa_esp_package_store_commit_install(file->source,approval);
    }
    return status;
}

bool pxa_esp_package_store_preview_file(const char *source_path,
                                        pxa_esp_package_preview_t *preview) {
    char verified_root[PXA_ESP_PACKAGE_MAX_PATH];
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
    if(!preview_manifest(manifest,preview))goto done;
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
        pxa_esp_package_entry_t *entry=find_entry(identity);char relative[130];
        /* New homes derive their namespace from the signed publisher root.
         * Verifying a display binding must not allocate a per-app NVS row. */
        if(!g_store->legacy_ready || (entry && entry->volume_local) ||
            pxa_app_locators_find(&g_store->locators,binding.root,binding.app_id,NULL,relative,sizeof(relative))!=PXA_STATUS_OK)
            status=PXA_STATUS_OK;
        else {
            status=legacy_layout_flags(binding.root,binding.app_id,&flags);
            if(status==PXA_STATUS_OK)status=pxa_app_locators_reserve(&g_store->locators,PXA_LOCATOR_VERIFIED_NAMESPACE,namespace_key,
                binding.profile.handle,binding.root,binding.app_id,app_key,flags);
        }
    }
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
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
/* Only the activation owner calls this, after stopping the old app. Each move
 * is preceded by a durable intent; boot/relaunch can finish an interrupted move.
 * Legacy adapters remain available for packages without a publisher profile. */
pxa_status_t pxa_esp_package_store_migrate_storage(const char *identity,char *root_out,size_t capacity) {
    if(!store_is_initialized() || !identity || !root_out || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)){release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    pxa_esp_package_entry_t *entry=find_entry(identity);pxa_status_t status=PXA_STATUS_NOT_FOUND;
    if(!entry || !entry->installed)goto migration_done;
    if(entry->ambiguous) {status=PXA_STATUS_BAD_STATE;goto migration_done;}
    if(entry->volume_local) {status=PXA_STATUS_OK;goto migration_copy;}
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
    status=PXA_STATUS_OK;
migration_copy:
    {int n=snprintf(root_out,capacity,"%s",entry->root);if(n<0 || (size_t)n>=capacity)status=PXA_STATUS_RESOURCE_LIMIT;}
migration_done:
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);
    if(status!=PXA_STATUS_OK)root_out[0]=0;
    return status;
}

/* Called with metadata lock held. An exact identity can also address retained
 * data after archiving. A live target is checked separately by move-data. */
static pxa_status_t storage_paths_locked(const char *identity,int allow_legacy,
    char *private_root,size_t private_capacity,char *media_relative,size_t media_capacity) {
    uint8_t publisher[32];char app_id[65],key[130],relative[130];uint16_t flags=0;
    pxa_esp_package_entry_t *entry=find_entry(identity);
    /* A volume-local private filesystem is a leased session, possibly an
     * unmounted image. Never fabricate an old Flash path for it. */
    if(entry && entry->volume_local)return PXA_STATUS_UNSUPPORTED;
    if(entry && entry->has_publisher_key_id) {
        memcpy(publisher,entry->publisher_key_id,32);
        snprintf(app_id,sizeof(app_id),"%s",entry->id);
    } else {
        if(!allow_legacy)return PXA_STATUS_NOT_FOUND;
        if(strlen(identity)<66 || identity[64]!=':' || !safe_app_id(identity+65))return PXA_STATUS_NOT_FOUND;
        for(unsigned i=0;i<32;++i) {
            unsigned value=0;
            for(unsigned j=0;j<2;++j) {
                char c=identity[i*2+j];
                if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return PXA_STATUS_INVALID_ARGUMENT;
                value=value*16+(c<='9'?c-'0':c-'a'+10);
            }
            publisher[i]=(uint8_t)value;
        }
        snprintf(app_id,sizeof(app_id),"%s",identity+65);
    }
    if(!filesystem_identity(key,sizeof(key),publisher,app_id))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_app_locators_find(&g_store->locators,publisher,app_id,&flags,relative,sizeof(relative));
    if(status==PXA_STATUS_OK) {
        if(!(flags&PXA_LOCATOR_DATA_LEGACY))snprintf(key,sizeof(key),"%s",relative);
    } else if(status==PXA_STATUS_NOT_FOUND && allow_legacy) {
        if(!canonical_identity(relative,sizeof(relative),publisher,app_id))return PXA_STATUS_INVALID_ARGUMENT;
        relative[64]='/';
    } else return status;
    if(entry && entry->storage_key[0])snprintf(key,sizeof(key),"%s",entry->storage_key);
    int n=snprintf(private_root,private_capacity,"%s/%s",g_store->data_root,key);
    if(n<0 || (size_t)n>=private_capacity)return PXA_STATUS_RESOURCE_LIMIT;
    n=snprintf(media_relative,media_capacity,"PXA/media/%s",relative);
    return n<0 || (size_t)n>=media_capacity?PXA_STATUS_RESOURCE_LIMIT:PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_debug_storage_paths(const char *identity,
    char *private_root,size_t private_capacity,char *media_relative,size_t media_capacity) {
    if(!identity || !private_root || !media_relative || !store_is_initialized())return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->metadata_lock))return PXA_STATUS_UNAVAILABLE;
    pxa_status_t status=storage_paths_locked(identity,1,private_root,private_capacity,media_relative,media_capacity);
    release_lock(g_store->metadata_lock);
    if(status!=PXA_STATUS_OK){if(private_capacity)private_root[0]=0;if(media_capacity)media_relative[0]=0;}
    return status;
}

pxa_status_t pxa_esp_package_store_storage_paths(const char *identity,
    char *private_root,size_t private_capacity,char *media_relative,size_t media_capacity) {
    if(!identity || !private_root || !media_relative || !store_is_initialized())return PXA_STATUS_INVALID_ARGUMENT;
    if(!take_lock(g_store->metadata_lock))return PXA_STATUS_UNAVAILABLE;
    pxa_esp_package_entry_t *entry=find_entry(identity);
    /* A verified legacy package may predate signed publisher profiles and
     * have no NVS locator row. Its existing composite private namespace and
     * fixed publisher/App media directory remain valid until adoption. This
     * fallback is only available for an installed catalog identity. */
    pxa_status_t status=entry && entry->installed?
        storage_paths_locked(identity,1,private_root,private_capacity,media_relative,media_capacity):PXA_STATUS_NOT_FOUND;
    release_lock(g_store->metadata_lock);
    if(status!=PXA_STATUS_OK){if(private_capacity)private_root[0]=0;if(media_capacity)media_relative[0]=0;}
    return status;
}

/* Trusted developer operation, run only by the runtime owner while idle.
 * New identity must be installed and verified; source must have the exact same
 * publisher root. Move only the app's data/, never capability/grant metadata. */
pxa_status_t pxa_esp_package_store_move_data(const char *from,const char *to) {
    if(!from || !to || strlen(from)<66 || strlen(to)<66 || from[64]!=':' || to[64]!=':' ||
        strncmp(from,to,65) || !strcmp(from,to))return PXA_STATUS_DENIED;
    if(!store_is_initialized() || !take_lock(g_store->transaction_lock))return PXA_STATUS_UNAVAILABLE;
    if(!take_lock(g_store->metadata_lock)){release_lock(g_store->transaction_lock);return PXA_STATUS_UNAVAILABLE;}
    pxa_status_t status=PXA_STATUS_NOT_FOUND;
    pxa_esp_package_entry_t *target=find_entry(to);
    char source_root[PXA_ESP_PACKAGE_ROOT_BYTES],target_root[PXA_ESP_PACKAGE_ROOT_BYTES],media[140];
    if(!target || !target->installed || !target->has_publisher_key_id)goto done;
    status=storage_paths_locked(from,1,source_root,sizeof(source_root),media,sizeof(media));
    if(status!=PXA_STATUS_OK)goto done;
    status=storage_paths_locked(to,0,target_root,sizeof(target_root),media,sizeof(media));
    if(status==PXA_STATUS_OK)status=pxa_private_data_move(source_root,target_root,from);
done:
    release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return status;
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
    if(entry && entry->ambiguous) {
        release_lock(g_store->metadata_lock);release_lock(g_store->transaction_lock);return false;
    }
    if(entry && entry->volume_local) {
        pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home={0};
        uint64_t epoch=entry->mount_epoch,generation=entry->install_generation;
        uint8_t install_id[16];memcpy(install_id,entry->install_id,16);
        snprintf(canonical,sizeof(canonical),"%s",entry->identity);
        snprintf(storage_key,sizeof(storage_key),"%s",entry->storage_key);
        release_lock(g_store->metadata_lock);
        status=retry_volume_cleanup();
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(epoch,&owner);
        if(status==PXA_STATUS_OK)status=idle_volume_home(epoch,storage_key);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_remove(owner,storage_key,generation,keep_data,&home);
        status=close_volume_operation(&owner,status);
        (void)pxa_esp_storage_refresh_volume(epoch);
        if(take_lock(g_store->metadata_lock)) {
            entry=find_entry(canonical);
            if(entry && entry->volume_local && entry->mount_epoch==epoch && !memcmp(entry->install_id,install_id,16)) {
                if(status==PXA_STATUS_OK && !keep_data) {
                    remove_entry_at((size_t)(entry-g_store->entries));
                    (void)scan_inbox(0);
                } else if(home.record.generation) {
                    entry->install_generation=home.record.generation;entry->archived=1;
                    entry->installed=0;entry->enabled=0;entry->root[0]=0;
                }
            }
            release_lock(g_store->metadata_lock);
        } else status=PXA_STATUS_UNAVAILABLE;
        release_lock(g_store->transaction_lock);return status==PXA_STATUS_OK;
    }
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
    if(entry && entry->ambiguous) {release_lock(g_store->metadata_lock);goto done;}
    if(entry && entry->volume_local) {
        pxa_esp_installations_t *owner=NULL;pxa_posix_app_home_t home={0};
        uint64_t epoch=entry->mount_epoch,generation=entry->install_generation,data_generation=entry->data_generation;
        uint8_t install_id[16];memcpy(install_id,entry->install_id,sizeof(install_id));
        snprintf(canonical,sizeof(canonical),"%s",entry->identity);
        snprintf(storage_key,sizeof(storage_key),"%s",entry->storage_key);
        release_lock(g_store->metadata_lock);
        status=retry_volume_cleanup();
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(epoch,&owner);
        if(status==PXA_STATUS_OK)status=idle_volume_home(epoch,storage_key);
        if(status==PXA_STATUS_OK)status=pxa_esp_installations_reset_data(owner,storage_key,generation,data_generation,&home);
        status=close_volume_operation(&owner,status);
        (void)pxa_esp_storage_refresh_volume(epoch);
        if(take_lock(g_store->metadata_lock)) {
            entry=find_entry(canonical);
            if(entry && entry->volume_local && entry->mount_epoch==epoch &&
                !memcmp(entry->install_id,install_id,16) && home.record.generation) {
                entry->install_generation=home.record.generation;entry->data_generation=home.record.data_generation;
                entry->granted_permission_count=0;entry->granted_permissions=0;entry->permission_summary_known=0;
            }
            release_lock(g_store->metadata_lock);
        } else status=PXA_STATUS_UNAVAILABLE;
        success=status==PXA_STATUS_OK;goto done;
    }
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
    uint8_t volume_local=0,manifest_sha256[32];
    if (!store_is_initialized() || identity == NULL || root == NULL ||
        manifest_workspace == NULL || encoded == NULL ||
        manifest == NULL) {
        return false;
    }
    *manifest = NULL;
    if (!take_lock(g_store->metadata_lock)) return false;
    entry = find_entry(identity);
    if (entry == NULL || !entry->installed || !entry->has_publisher_key_id || entry->ambiguous) {
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
    volume_local=entry->volume_local;
    if(volume_local)memcpy(manifest_sha256,entry->manifest_sha256,32);
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
        if(status==PXA_STATUS_OK && volume_local) {
            uint8_t hash[32];
            if(pxa_esp_mbedtls_sha256((*manifest)->encoded.data,(*manifest)->encoded.size,hash)!=PXA_STATUS_OK ||
                memcmp(hash,manifest_sha256,32))status=PXA_STATUS_DENIED;
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
    record->archived=entry->volume_local && entry->archived;
    record->has_private_data = (entry->installed || record->archived) && entry->has_private_data;
    record->movable=(entry->installed || record->archived) && !entry->built_in && !entry->ambiguous;
    record->resources_movable=record->movable && entry->installed && entry->volume_local && entry->managed_media;
    record->legacy_storage=record->movable && !entry->volume_local;
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
    return package_entry_has_home(entry) || entry->staged;
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

/* Caller holds transaction + metadata locks. The provider and manifest are
 * borrowed until the owner closes; no authority falls back to Flash. */
static pxa_status_t maintenance_permissions(pxa_esp_package_entry_t *entry,
    pxa_esp_installations_t **owner,pxa_permission_store_t *provider) {
    *owner=NULL;memset(provider,0,sizeof(*provider));provider->struct_size=sizeof(*provider);
    if(!entry || entry->ambiguous)return PXA_STATUS_NOT_FOUND;
    if(!entry->volume_local) {
        provider->load=pxa_esp_permission_store_load;provider->save=pxa_esp_permission_store_save;
        return PXA_STATUS_OK;
    }
    pxa_status_t status=retry_volume_cleanup();pxa_posix_app_home_t home;
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(entry->mount_epoch,owner);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_lookup(*owner,entry->storage_key,&home);
    if(status==PXA_STATUS_OK && (memcmp(home.record.install_id,entry->install_id,16) ||
        home.record.generation<entry->install_generation || memcmp(home.record.manifest_sha256,entry->manifest_sha256,32)))
        status=PXA_STATUS_BAD_STATE;
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_permissions(*owner,&home,provider);
    return status;
}
static bool cache_permission_summary(pxa_esp_package_entry_t *entry,const pxa_package_manifest_t *manifest,
    const pxa_permission_store_t *provider) {
    uint64_t bits=0;uint8_t count=0;
    if(!entry || !manifest || !provider || !provider->load || manifest->permission_count>64)return false;
    entry->granted_permissions=0;entry->granted_permission_count=0;entry->permission_summary_known=0;
    pxa_bytes_t identity={(const uint8_t *)entry->identity,strlen(entry->identity)};
    for(uint16_t i=0;i<manifest->permission_count;++i) {
        pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
        pxa_status_t status=provider->load(provider->context,identity,manifest->permissions[i].name,
            manifest->permissions[i].scope,&decision);
        if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return false;
        if(decision==PXA_PERMISSION_ALLOW) {bits|=UINT64_C(1)<<i;++count;}
    }
    entry->permission_count=(uint8_t)manifest->permission_count;
    entry->granted_permission_count=count;entry->granted_permissions=bits;entry->permission_summary_known=1;
    return true;
}

size_t pxa_esp_package_store_visit_permissions(
    const char *identity, pxa_esp_package_permission_visitor_fn visitor,
    void *user_data) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    size_t count = 0;
    size_t index;
    pxa_esp_installations_t *owner=NULL;pxa_permission_store_t provider;
    if (!store_is_initialized() || identity == NULL || visitor == NULL ||
        !take_lock(g_store->transaction_lock)) {
        return 0;
    }
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return 0;}
    entry = find_entry(identity);
    if (!parse_entry_manifest(entry, &manifest)) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return 0;
    }
    if(entry->volume_local && !entry->permission_summary_known) {
        if(maintenance_permissions(entry,&owner,&provider)==PXA_STATUS_OK)
            (void)cache_permission_summary(entry,manifest,&provider);
        (void)close_volume_operation(&owner,PXA_STATUS_OK);
    }
    for (index = 0; index < manifest->permission_count; ++index) {
        const pxa_package_permission_t *permission =
            &manifest->permissions[index];
        pxa_esp_package_permission_info_t info;
        memset(&info, 0, sizeof(info));
        copy_bytes_text(info.name, sizeof(info.name), permission->name);
        copy_scope_text(info.scope, sizeof(info.scope), permission->scope);
        info.required = permission->required != 0;
        info.granted = entry->volume_local?
            (index<64 && !!(entry->granted_permissions&(UINT64_C(1)<<index))):permission_granted(entry->identity, permission);
        ++count;
        if (!visitor(&info, user_data)) break;
    }
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return count;
}

bool pxa_esp_package_store_set_permission(const char *identity,
                                          size_t permission_index,
                                          bool granted) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    const pxa_package_permission_t *permission;
    pxa_status_t status;
    pxa_esp_installations_t *owner=NULL;pxa_permission_store_t provider;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->transaction_lock)) {
        return false;
    }
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return false;}
    entry = find_entry(identity);
    if ((entry && entry->ambiguous) || !parse_entry_manifest(entry, &manifest) ||
        permission_index >= manifest->permission_count) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    permission = &manifest->permissions[permission_index];
    const char *stage="open-private-policy";
    status=maintenance_permissions(entry,&owner,&provider);
    if(status==PXA_STATUS_OK) {stage="save-private-policy";status = provider.save(
               provider.context,
               (pxa_bytes_t){(const uint8_t *)entry->identity,
                             strlen(entry->identity)},
               permission->name, permission->scope,
               granted ? PXA_PERMISSION_ALLOW : PXA_PERMISSION_DENY);}
    if (status == PXA_STATUS_OK) {
        stage="refresh-policy-view";
        if(!cache_permission_summary(entry,manifest,&provider))status=PXA_STATUS_UNAVAILABLE;
    }
    if(status==PXA_STATUS_OK)stage="close-private-policy";
    status=close_volume_operation(&owner,status);
    if(status!=PXA_STATUS_OK) {
        if(entry->volume_local) {entry->permission_summary_known=0;entry->granted_permissions=0;entry->granted_permission_count=0;}
        ESP_LOGW(PXA_ESP_PACKAGE_TAG,"Permission update failed: app=%s stage=%s status=%ld errno=%d",
            entry->identity,stage,(long)status,errno);
    }
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return status == PXA_STATUS_OK;
}

bool pxa_esp_package_store_refresh_permission_summary(const char *identity) {
    pxa_esp_package_entry_t *entry;
    pxa_package_manifest_t *manifest = NULL;
    if (!store_is_initialized() || identity == NULL ||
        !take_lock(g_store->transaction_lock)) {
        return false;
    }
    if(!take_lock(g_store->metadata_lock)) {release_lock(g_store->transaction_lock);return false;}
    entry = find_entry(identity);
    if (!parse_entry_manifest(entry, &manifest)) {
        release_lock(g_store->metadata_lock);
        release_lock(g_store->transaction_lock);
        return false;
    }
    pxa_esp_installations_t *owner=NULL;pxa_permission_store_t provider;
    pxa_status_t status=maintenance_permissions(entry,&owner,&provider);
    bool success=status==PXA_STATUS_OK && cache_permission_summary(entry,manifest,&provider);
    status=close_volume_operation(&owner,status);
    release_lock(g_store->metadata_lock);
    release_lock(g_store->transaction_lock);
    return success && status==PXA_STATUS_OK;
}

bool pxa_esp_package_store_refresh_permission_summary_with_storage(const char *identity,pxa_esp_app_storage_t *storage) {
    if(!storage)return pxa_esp_package_store_refresh_permission_summary(identity);
    if(!store_is_initialized() || !identity || !take_lock(g_store->metadata_lock))return false;
    pxa_esp_package_entry_t *entry=find_entry(identity);pxa_package_manifest_t *manifest=NULL;pxa_permission_store_t provider;
    bool success=entry && entry->volume_local && !entry->ambiguous && parse_entry_manifest(entry,&manifest) &&
        pxa_esp_app_storage_permissions(storage,&provider)==PXA_STATUS_OK && cache_permission_summary(entry,manifest,&provider);
    release_lock(g_store->metadata_lock);return success;
}

pxa_posix_installer_t *pxa_esp_package_store_installer(void) {
    return g_store != NULL ? g_store->installer : NULL;
}

#endif /* ESP_PLATFORM */
