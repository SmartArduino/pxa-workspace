/* ESP32 Layer 2 permission persistence over the assets LittleFS partition.
 * Each application has two checksummed policy slots. Only allow grants are
 * stored; an absent tuple defaults to deny. */

#include "pxa_esp_permission_store.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "pxa/esp/pxa_esp_mbedtls.h"

#ifndef CONFIG_PXA_MOUNT_POINT
#define CONFIG_PXA_MOUNT_POINT "/assets"
#endif
#ifndef CONFIG_PXA_STATE_ROOT
#define CONFIG_PXA_STATE_ROOT "pxa-state"
#endif

#define PXA_ESP_PERMISSION_POLICY_ROOT                                    \
    CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT "/policy"
#define PXA_ESP_PERMISSION_STATE_ROOT                                     \
    CONFIG_PXA_MOUNT_POINT "/" CONFIG_PXA_STATE_ROOT
#define PXA_ESP_PERMISSION_HEADER_BYTES 32
#define PXA_ESP_PERMISSION_RECORD_BYTES 20
#define PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES 16
#define PXA_ESP_PERMISSION_FILE_DIGEST_BYTES 12
#define PXA_ESP_PERMISSION_CHECKSUM_BYTES 16
#define PXA_ESP_PERMISSION_PATH_BYTES 192

/* Legacy per-tuple NVS layout. Keep these constants stable so existing
 * grants can be migrated lazily without enumerating the NVS namespace. */
#define PXA_ESP_PERMISSION_LEGACY_NS "pxa_apps"
#define PXA_ESP_PERMISSION_LEGACY_KEY_PREFIX "p_"
#define PXA_ESP_PERMISSION_LEGACY_KEY_DIGEST_BYTES 6
#define PXA_ESP_PERMISSION_LEGACY_KEY_LEN                                \
    (sizeof(PXA_ESP_PERMISSION_LEGACY_KEY_PREFIX) - 1 +                  \
     PXA_ESP_PERMISSION_LEGACY_KEY_DIGEST_BYTES * 2)

static const char *PXA_ESP_PERMISSION_TAG = "PxaPermissionStore";
static const uint8_t PXA_ESP_PERMISSION_MAGIC[8] = {
    'P', 'X', 'A', 'P', 'R', 'M', 1, 0,
};
static pthread_mutex_t g_permission_store_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
} pxa_esp_permission_record_t;

typedef struct {
    uint32_t generation;
    pxa_esp_permission_record_t *records;
    uint16_t count;
    uint16_t capacity;
} pxa_esp_permission_snapshot_t;

typedef struct {
    pxa_esp_permission_snapshot_t selected;
    pxa_esp_permission_snapshot_t other;
} pxa_esp_permission_workspace_t;

typedef struct {
    pxa_esp_permission_workspace_t workspace;
    pxa_esp_permission_snapshot_t legacy_checked;
    uint8_t identity_digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    int active_slot;
    uint8_t has_snapshot;
    uint8_t valid;
} pxa_esp_permission_cache_t;

/* Process-owned, mutex-protected, one-identity cache. Package policy files are
 * private to this store; every supported write updates the cache on commit. */
static pxa_esp_permission_cache_t *g_permission_cache;
static uint8_t g_legacy_namespace_absent;

static void *allocate_file_buffer(size_t size);

static void permission_snapshot_release(
    pxa_esp_permission_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    heap_caps_free(snapshot->records);
    memset(snapshot, 0, sizeof(*snapshot));
}

static int permission_snapshot_reserve(pxa_esp_permission_snapshot_t *snapshot,
                                       uint16_t required) {
    pxa_esp_permission_record_t *resized;
    size_t bytes;
    if (snapshot == NULL) return 0;
    if (required <= snapshot->capacity) return 1;
    bytes = (size_t)required * sizeof(snapshot->records[0]);
    if (snapshot->records == NULL) {
        resized = (pxa_esp_permission_record_t *)allocate_file_buffer(bytes);
    } else {
        resized = (pxa_esp_permission_record_t *)heap_caps_realloc(
            snapshot->records, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (resized == NULL) {
            resized = (pxa_esp_permission_record_t *)heap_caps_realloc(
                snapshot->records, bytes, MALLOC_CAP_8BIT);
        }
    }
    if (resized == NULL) return 0;
    snapshot->records = resized;
    snapshot->capacity = required;
    return 1;
}

static void permission_snapshot_clear(pxa_esp_permission_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    snapshot->generation = 0;
    snapshot->count = 0;
}

static int permission_snapshot_copy(pxa_esp_permission_snapshot_t *destination,
                                    const pxa_esp_permission_snapshot_t *source) {
    if (destination == NULL || source == NULL ||
        !permission_snapshot_reserve(destination, source->count)) {
        return 0;
    }
    destination->generation = source->generation;
    destination->count = source->count;
    if (source->count != 0) {
        memcpy(destination->records, source->records,
               (size_t)source->count * sizeof(destination->records[0]));
    }
    return 1;
}

static void permission_snapshot_swap(pxa_esp_permission_snapshot_t *left,
                                     pxa_esp_permission_snapshot_t *right) {
    pxa_esp_permission_snapshot_t temporary;
    if (left == NULL || right == NULL) return;
    temporary = *left;
    *left = *right;
    *right = temporary;
}

static void permission_cache_clear(pxa_esp_permission_cache_t *cache) {
    if (cache == NULL) return;
    permission_snapshot_release(&cache->workspace.selected);
    permission_snapshot_release(&cache->workspace.other);
    permission_snapshot_release(&cache->legacy_checked);
    memset(cache, 0, sizeof(*cache));
    cache->active_slot = -1;
}

typedef enum {
    PXA_ESP_PERMISSION_SLOT_MISSING = 0,
    PXA_ESP_PERMISSION_SLOT_VALID = 1,
    PXA_ESP_PERMISSION_SLOT_INVALID = -1,
    PXA_ESP_PERMISSION_SLOT_IO_ERROR = -2,
    PXA_ESP_PERMISSION_SLOT_RESOURCE_LIMIT = -3,
} pxa_esp_permission_slot_state_t;

static uint16_t read_u16_le(const uint8_t *data) {
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16_le(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void bytes_to_hex(const uint8_t *bytes, size_t size, char *output) {
    static const char hex[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < size; ++index) {
        output[index * 2] = hex[bytes[index] >> 4];
        output[index * 2 + 1] = hex[bytes[index] & 0x0f];
    }
    output[size * 2] = '\0';
}

static int sha256_begin(mbedtls_md_context_t *sha) {
    const mbedtls_md_info_t *info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(sha);
    if (info == NULL || mbedtls_md_setup(sha, info, 0) != 0 ||
        mbedtls_md_starts(sha) != 0) {
        mbedtls_md_free(sha);
        return 0;
    }
    return 1;
}

static int sha256_finish(mbedtls_md_context_t *sha, uint8_t *digest) {
    int success = mbedtls_md_finish(sha, digest) == 0;
    mbedtls_md_free(sha);
    return success;
}

static int hash_identity(pxa_bytes_t identity, uint8_t *digest) {
    mbedtls_md_context_t sha;
    if (!sha256_begin(&sha)) return 0;
    if (mbedtls_md_update(&sha, identity.data, identity.size) != 0) {
        mbedtls_md_free(&sha);
        return 0;
    }
    return sha256_finish(&sha, digest);
}

static int hash_tuple_field(mbedtls_md_context_t *sha, uint8_t tag,
                            pxa_bytes_t value) {
    uint8_t prefix[5];
    uint32_t size;
    if (value.size > UINT32_MAX ||
        (value.size != 0 && value.data == NULL)) {
        return 0;
    }
    size = (uint32_t)value.size;
    prefix[0] = tag;
    write_u32_le(prefix + 1, size);
    if (mbedtls_md_update(sha, prefix, sizeof(prefix)) != 0) return 0;
    return value.size == 0 ||
           mbedtls_md_update(sha, value.data, value.size) == 0;
}

static int derive_tuple_digest(pxa_bytes_t name, pxa_bytes_t scope,
                               uint8_t *tuple_digest) {
    uint8_t digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    mbedtls_md_context_t sha;
    if (!sha256_begin(&sha)) return 0;
    if (!hash_tuple_field(&sha, 1, name) ||
        !hash_tuple_field(&sha, 2, scope)) {
        mbedtls_md_free(&sha);
        return 0;
    }
    if (!sha256_finish(&sha, digest)) {
        return 0;
    }
    memcpy(tuple_digest, digest, PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES);
    return 1;
}

static int derive_legacy_key(pxa_bytes_t identity, pxa_bytes_t name,
                             pxa_bytes_t scope, char *key) {
    uint8_t digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    mbedtls_md_context_t sha;
    if (!sha256_begin(&sha)) return 0;
    if ((identity.size != 0 &&
         mbedtls_md_update(&sha, identity.data, identity.size) != 0) ||
        (name.size != 0 &&
         mbedtls_md_update(&sha, name.data, name.size) != 0) ||
        (scope.size != 0 &&
         mbedtls_md_update(&sha, scope.data, scope.size) != 0)) {
        mbedtls_md_free(&sha);
        return 0;
    }
    if (!sha256_finish(&sha, digest)) {
        return 0;
    }
    memcpy(key, PXA_ESP_PERMISSION_LEGACY_KEY_PREFIX,
           sizeof(PXA_ESP_PERMISSION_LEGACY_KEY_PREFIX) - 1);
    bytes_to_hex(digest, PXA_ESP_PERMISSION_LEGACY_KEY_DIGEST_BYTES,
                 key + sizeof(PXA_ESP_PERMISSION_LEGACY_KEY_PREFIX) - 1);
    return 1;
}

static int build_slot_path(pxa_bytes_t identity, int slot, char *path,
                           size_t capacity) {
    uint8_t digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    char hex[PXA_ESP_PERMISSION_FILE_DIGEST_BYTES * 2 + 1];
    int length;
    if (!hash_identity(identity, digest)) return 0;
    bytes_to_hex(digest, PXA_ESP_PERMISSION_FILE_DIGEST_BYTES, hex);
    length = snprintf(path, capacity, "%s/p_%s.%c",
                      PXA_ESP_PERMISSION_POLICY_ROOT, hex,
                      slot == 0 ? 'a' : 'b');
    return length > 0 && (size_t)length < capacity;
}

static int ensure_directory(const char *path) {
    struct stat state;
    if (stat(path, &state) == 0) return S_ISDIR(state.st_mode);
    if (errno != ENOENT) return 0;
    if (mkdir(path, 0755) == 0) return 1;
    if (errno != EEXIST || stat(path, &state) != 0) return 0;
    return S_ISDIR(state.st_mode);
}

static int ensure_policy_root(void) {
    return ensure_directory(PXA_ESP_PERMISSION_STATE_ROOT) &&
           ensure_directory(PXA_ESP_PERMISSION_POLICY_ROOT);
}

static void *allocate_file_buffer(size_t size) {
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return buffer;
}

static int calculate_checksum(const uint8_t *bytes, size_t size,
                              uint8_t *checksum) {
    uint8_t digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    mbedtls_md_context_t sha;
    if (size < PXA_ESP_PERMISSION_HEADER_BYTES || !sha256_begin(&sha)) {
        return 0;
    }
    if (mbedtls_md_update(&sha, bytes, 16) != 0 ||
        (size > PXA_ESP_PERMISSION_HEADER_BYTES &&
         mbedtls_md_update(
             &sha, bytes + PXA_ESP_PERMISSION_HEADER_BYTES,
             size - PXA_ESP_PERMISSION_HEADER_BYTES) != 0)) {
        mbedtls_md_free(&sha);
        return 0;
    }
    if (!sha256_finish(&sha, digest)) {
        return 0;
    }
    memcpy(checksum, digest, PXA_ESP_PERMISSION_CHECKSUM_BYTES);
    return 1;
}

static pxa_esp_permission_slot_state_t read_slot(
    pxa_bytes_t identity, int slot, pxa_esp_permission_snapshot_t *snapshot) {
    char path[PXA_ESP_PERMISSION_PATH_BYTES];
    struct stat state;
    uint8_t expected_checksum[PXA_ESP_PERMISSION_CHECKSUM_BYTES];
    uint8_t *bytes;
    size_t size;
    size_t index;
    size_t bytes_read;
    FILE *file;
    int close_status;
    int saved_errno;
    if (!build_slot_path(identity, slot, path, sizeof(path))) {
        return PXA_ESP_PERMISSION_SLOT_IO_ERROR;
    }
    if (stat(path, &state) != 0) {
        saved_errno = errno;
        if (saved_errno == ENOENT) return PXA_ESP_PERMISSION_SLOT_MISSING;
        ESP_LOGW(PXA_ESP_PERMISSION_TAG, "Cannot stat policy slot %c: errno=%d",
                 slot == 0 ? 'A' : 'B', saved_errno);
        return PXA_ESP_PERMISSION_SLOT_IO_ERROR;
    }
    if (state.st_size < PXA_ESP_PERMISSION_HEADER_BYTES) {
        ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                 "Ignore invalid policy slot %c size=%ld",
                 slot == 0 ? 'A' : 'B', (long)state.st_size);
        return PXA_ESP_PERMISSION_SLOT_INVALID;
    }
    size = (size_t)state.st_size;
    bytes = (uint8_t *)allocate_file_buffer(size);
    if (bytes == NULL) return PXA_ESP_PERMISSION_SLOT_IO_ERROR;
    file = fopen(path, "rb");
    if (file == NULL) {
        saved_errno = errno;
        heap_caps_free(bytes);
        ESP_LOGW(PXA_ESP_PERMISSION_TAG, "Cannot open policy slot %c: errno=%d",
                 slot == 0 ? 'A' : 'B', saved_errno);
        return PXA_ESP_PERMISSION_SLOT_IO_ERROR;
    }
    errno = 0;
    bytes_read = fread(bytes, 1, size, file);
    saved_errno = bytes_read == size ? 0 : (errno != 0 ? errno : EIO);
    close_status = fclose(file);
    if (bytes_read != size || close_status != 0) {
        if (saved_errno == 0) saved_errno = errno != 0 ? errno : EIO;
        heap_caps_free(bytes);
        ESP_LOGW(PXA_ESP_PERMISSION_TAG, "Cannot read policy slot %c: errno=%d",
                 slot == 0 ? 'A' : 'B', saved_errno);
        return PXA_ESP_PERMISSION_SLOT_IO_ERROR;
    }
    if (memcmp(bytes, PXA_ESP_PERMISSION_MAGIC,
               sizeof(PXA_ESP_PERMISSION_MAGIC)) != 0 ||
        read_u16_le(bytes + 14) != PXA_ESP_PERMISSION_RECORD_BYTES ||
        size != PXA_ESP_PERMISSION_HEADER_BYTES +
                    (size_t)read_u16_le(bytes + 12) *
                        PXA_ESP_PERMISSION_RECORD_BYTES ||
        !calculate_checksum(bytes, size, expected_checksum) ||
        memcmp(bytes + 16, expected_checksum,
               PXA_ESP_PERMISSION_CHECKSUM_BYTES) != 0) {
        heap_caps_free(bytes);
        ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                 "Ignore corrupt policy slot %c", slot == 0 ? 'A' : 'B');
        return PXA_ESP_PERMISSION_SLOT_INVALID;
    }
    for (index = 0; index < read_u16_le(bytes + 12); ++index) {
        const uint8_t *record = bytes + PXA_ESP_PERMISSION_HEADER_BYTES +
                                index * PXA_ESP_PERMISSION_RECORD_BYTES;
        if (record[16] != PXA_PERMISSION_ALLOW || record[17] != 0 ||
            record[18] != 0 || record[19] != 0 ||
            (index != 0 &&
             memcmp(record - PXA_ESP_PERMISSION_RECORD_BYTES, record,
                    PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES) >= 0)) {
            heap_caps_free(bytes);
            ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                     "Ignore invalid policy records in slot %c",
                     slot == 0 ? 'A' : 'B');
            return PXA_ESP_PERMISSION_SLOT_INVALID;
        }
    }
    if (!permission_snapshot_reserve(snapshot, read_u16_le(bytes + 12))) {
        heap_caps_free(bytes);
        return PXA_ESP_PERMISSION_SLOT_RESOURCE_LIMIT;
    }
    permission_snapshot_clear(snapshot);
    snapshot->generation = read_u32_le(bytes + 8);
    snapshot->count = read_u16_le(bytes + 12);
    for (index = 0; index < snapshot->count; ++index) {
        const uint8_t *record = bytes + PXA_ESP_PERMISSION_HEADER_BYTES +
                                index * PXA_ESP_PERMISSION_RECORD_BYTES;
        memcpy(snapshot->records[index].digest, record,
               PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES);
    }
    heap_caps_free(bytes);
    return PXA_ESP_PERMISSION_SLOT_VALID;
}

static int generation_is_newer(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) > 0;
}

static pxa_status_t load_best_snapshot(
    pxa_bytes_t identity, pxa_esp_permission_snapshot_t *snapshot,
    pxa_esp_permission_snapshot_t *other, int *active_slot, int *found) {
    pxa_esp_permission_slot_state_t left_state;
    pxa_esp_permission_slot_state_t right_state;
    permission_snapshot_clear(snapshot);
    permission_snapshot_clear(other);
    left_state = read_slot(identity, 0, snapshot);
    right_state = read_slot(identity, 1, other);
    *found = 0;
    *active_slot = -1;
    if (left_state == PXA_ESP_PERMISSION_SLOT_VALID &&
        right_state == PXA_ESP_PERMISSION_SLOT_VALID) {
        if (generation_is_newer(other->generation, snapshot->generation)) {
            permission_snapshot_swap(snapshot, other);
            *active_slot = 1;
        } else {
            *active_slot = 0;
        }
        *found = 1;
        return PXA_STATUS_OK;
    }
    if (left_state == PXA_ESP_PERMISSION_SLOT_VALID) {
        *active_slot = 0;
        *found = 1;
        return PXA_STATUS_OK;
    }
    if (right_state == PXA_ESP_PERMISSION_SLOT_VALID) {
        permission_snapshot_swap(snapshot, other);
        *active_slot = 1;
        *found = 1;
        return PXA_STATUS_OK;
    }
    if (left_state == PXA_ESP_PERMISSION_SLOT_RESOURCE_LIMIT ||
        right_state == PXA_ESP_PERMISSION_SLOT_RESOURCE_LIMIT) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    if (left_state == PXA_ESP_PERMISSION_SLOT_IO_ERROR ||
        right_state == PXA_ESP_PERMISSION_SLOT_IO_ERROR) {
        return PXA_STATUS_INTERNAL;
    }
    return PXA_STATUS_OK;
}

static size_t find_record(const pxa_esp_permission_snapshot_t *snapshot,
                          const uint8_t *digest, int *found) {
    size_t low = 0;
    size_t high = snapshot->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int comparison = memcmp(snapshot->records[middle].digest, digest,
                                PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES);
        if (comparison < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    *found = low < snapshot->count &&
             memcmp(snapshot->records[low].digest, digest,
                    PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES) == 0;
    return low;
}

static pxa_status_t select_permission_cache(
    pxa_bytes_t identity, pxa_esp_permission_cache_t **cache_out) {
    pxa_esp_permission_cache_t *cache;
    uint8_t identity_digest[PXA_ESP_MBEDTLS_SHA256_BYTES];
    pxa_status_t status;
    int active_slot;
    int has_snapshot;
    if (cache_out == NULL || !hash_identity(identity, identity_digest)) {
        return PXA_STATUS_INTERNAL;
    }
    cache = g_permission_cache;
    if (cache == NULL) {
        cache = (pxa_esp_permission_cache_t *)allocate_file_buffer(
            sizeof(*cache));
        if (cache == NULL) return PXA_STATUS_RESOURCE_LIMIT;
        memset(cache, 0, sizeof(*cache));
        cache->active_slot = -1;
        g_permission_cache = cache;
        ESP_LOGD(PXA_ESP_PERMISSION_TAG,
                 "Permission snapshot cache allocated: %u bytes",
                 (unsigned)sizeof(*cache));
    }
    if (cache->valid &&
        memcmp(cache->identity_digest, identity_digest,
               sizeof(identity_digest)) == 0) {
        *cache_out = cache;
        return PXA_STATUS_OK;
    }

    permission_cache_clear(cache);
    status = load_best_snapshot(identity, &cache->workspace.selected,
                                &cache->workspace.other, &active_slot,
                                &has_snapshot);
    if (status != PXA_STATUS_OK) return status;
    memcpy(cache->identity_digest, identity_digest, sizeof(identity_digest));
    cache->active_slot = active_slot;
    cache->has_snapshot = has_snapshot != 0;
    cache->valid = 1;
    *cache_out = cache;
    return PXA_STATUS_OK;
}

static int permission_cache_legacy_checked(
    pxa_esp_permission_cache_t *cache, pxa_bytes_t name, pxa_bytes_t scope) {
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
    int found;
    if (cache == NULL || !derive_tuple_digest(name, scope, digest)) return 0;
    (void)find_record(&cache->legacy_checked, digest, &found);
    return found;
}

static void permission_cache_mark_legacy_checked(
    pxa_esp_permission_cache_t *cache, pxa_bytes_t name, pxa_bytes_t scope) {
    pxa_esp_permission_snapshot_t *checked;
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
    size_t position;
    int found;
    if (cache == NULL || !derive_tuple_digest(name, scope, digest)) return;
    checked = &cache->legacy_checked;
    position = find_record(checked, digest, &found);
    if (found || checked->count == UINT16_MAX ||
        !permission_snapshot_reserve(checked,
                                     (uint16_t)(checked->count + 1u))) {
        return;
    }
    memmove(&checked->records[position + 1], &checked->records[position],
            (checked->count - position) * sizeof(checked->records[0]));
    memcpy(checked->records[position].digest, digest, sizeof(digest));
    ++checked->count;
}

static void permission_cache_unmark_legacy_checked(
    pxa_esp_permission_cache_t *cache, pxa_bytes_t name, pxa_bytes_t scope) {
    pxa_esp_permission_snapshot_t *checked;
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
    size_t position;
    int found;
    if (cache == NULL || !derive_tuple_digest(name, scope, digest)) return;
    checked = &cache->legacy_checked;
    position = find_record(checked, digest, &found);
    if (!found) return;
    memmove(&checked->records[position], &checked->records[position + 1],
            (checked->count - position - 1u) * sizeof(checked->records[0]));
    --checked->count;
}

static pxa_status_t write_snapshot(
    pxa_bytes_t identity, int slot,
    const pxa_esp_permission_snapshot_t *snapshot) {
    char path[PXA_ESP_PERMISSION_PATH_BYTES];
    uint8_t checksum[PXA_ESP_PERMISSION_CHECKSUM_BYTES];
    uint8_t *bytes;
    size_t size;
    size_t index;
    FILE *file;
    int success;
    int saved_errno = 0;
    if (!ensure_policy_root() ||
        !build_slot_path(identity, slot, path, sizeof(path))) {
        return PXA_STATUS_INTERNAL;
    }
    size = PXA_ESP_PERMISSION_HEADER_BYTES +
           (size_t)snapshot->count * PXA_ESP_PERMISSION_RECORD_BYTES;
    bytes = (uint8_t *)allocate_file_buffer(size);
    if (bytes == NULL) return PXA_STATUS_RESOURCE_LIMIT;
    memset(bytes, 0, size);
    memcpy(bytes, PXA_ESP_PERMISSION_MAGIC, sizeof(PXA_ESP_PERMISSION_MAGIC));
    write_u32_le(bytes + 8, snapshot->generation);
    write_u16_le(bytes + 12, snapshot->count);
    write_u16_le(bytes + 14, PXA_ESP_PERMISSION_RECORD_BYTES);
    for (index = 0; index < snapshot->count; ++index) {
        uint8_t *record = bytes + PXA_ESP_PERMISSION_HEADER_BYTES +
                          index * PXA_ESP_PERMISSION_RECORD_BYTES;
        memcpy(record, snapshot->records[index].digest,
               PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES);
        record[16] = PXA_PERMISSION_ALLOW;
    }
    if (!calculate_checksum(bytes, size, checksum)) {
        heap_caps_free(bytes);
        return PXA_STATUS_INTERNAL;
    }
    memcpy(bytes + 16, checksum, sizeof(checksum));
    file = fopen(path, "wb");
    if (file == NULL) {
        saved_errno = errno;
        heap_caps_free(bytes);
        ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                 "Cannot open policy slot %c for write: errno=%d",
                 slot == 0 ? 'A' : 'B', saved_errno);
        return saved_errno == ENOSPC ? PXA_STATUS_QUOTA_EXCEEDED
                                     : PXA_STATUS_INTERNAL;
    }
    success = fwrite(bytes, 1, size, file) == size;
    if (success) success = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (!success) saved_errno = errno;
    if (fclose(file) != 0) {
        if (success) saved_errno = errno;
        success = 0;
    }
    heap_caps_free(bytes);
    if (!success) {
        (void)unlink(path);
        ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                 "Cannot persist policy slot %c: errno=%d",
                 slot == 0 ? 'A' : 'B', saved_errno);
        return saved_errno == ENOSPC ? PXA_STATUS_QUOTA_EXCEEDED
                                     : PXA_STATUS_INTERNAL;
    }
    return PXA_STATUS_OK;
}

static pxa_status_t policy_load(pxa_bytes_t identity, pxa_bytes_t name,
                                pxa_bytes_t scope,
                                pxa_permission_decision_t *decision) {
    pxa_esp_permission_cache_t *cache;
    pxa_esp_permission_snapshot_t *snapshot;
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
    int found;
    pxa_status_t status;
    if (!derive_tuple_digest(name, scope, digest)) return PXA_STATUS_INTERNAL;
    status = select_permission_cache(identity, &cache);
    if (status != PXA_STATUS_OK) return status;
    if (!cache->has_snapshot) return PXA_STATUS_NOT_FOUND;
    snapshot = &cache->workspace.selected;
    (void)find_record(snapshot, digest, &found);
    if (!found) return PXA_STATUS_NOT_FOUND;
    *decision = PXA_PERMISSION_ALLOW;
    return PXA_STATUS_OK;
}

static pxa_status_t policy_save(pxa_bytes_t identity, pxa_bytes_t name,
                                pxa_bytes_t scope,
                                pxa_permission_decision_t decision) {
    pxa_esp_permission_cache_t *cache;
    pxa_esp_permission_snapshot_t *current;
    pxa_esp_permission_snapshot_t *snapshot;
    uint8_t digest[PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES];
    size_t position;
    int found;
    int changed = 0;
    int target_slot;
    pxa_status_t status;
    pxa_status_t mirror_status;
    if (!derive_tuple_digest(name, scope, digest)) return PXA_STATUS_INTERNAL;
    status = select_permission_cache(identity, &cache);
    if (status != PXA_STATUS_OK) return status;
    current = &cache->workspace.selected;
    snapshot = &cache->workspace.other;
    if (!permission_snapshot_copy(snapshot, current)) {
        return PXA_STATUS_RESOURCE_LIMIT;
    }
    position = find_record(snapshot, digest, &found);
    if (decision == PXA_PERMISSION_ALLOW && !found) {
        if (snapshot->count == UINT16_MAX ||
            !permission_snapshot_reserve(
                snapshot, (uint16_t)(snapshot->count + 1u))) {
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        memmove(&snapshot->records[position + 1], &snapshot->records[position],
                (snapshot->count - position) * sizeof(snapshot->records[0]));
        memcpy(snapshot->records[position].digest, digest,
               PXA_ESP_PERMISSION_TUPLE_DIGEST_BYTES);
        ++snapshot->count;
        changed = 1;
    } else if (decision == PXA_PERMISSION_DENY && found) {
        memmove(&snapshot->records[position], &snapshot->records[position + 1],
                (snapshot->count - position - 1) *
                    sizeof(snapshot->records[0]));
        --snapshot->count;
        changed = 1;
    }
    if (!changed) return PXA_STATUS_OK;
    snapshot->generation =
        cache->has_snapshot ? current->generation + 1u : 1u;
    if (snapshot->generation == 0) snapshot->generation = 1;
    target_slot = cache->active_slot == 0 ? 1 : 0;
    status = write_snapshot(identity, target_slot, snapshot);
    if (status == PXA_STATUS_OK) {
        /* Once the new generation is durable, mirror it into the old slot.
         * This keeps a later single-slot corruption from restoring a stale
         * grant that the new generation revoked. */
        mirror_status = write_snapshot(identity, target_slot == 0 ? 1 : 0,
                                       snapshot);
        if (mirror_status != PXA_STATUS_OK) {
            ESP_LOGW(PXA_ESP_PERMISSION_TAG,
                     "Policy committed in slot %c; mirror update failed",
                     target_slot == 0 ? 'A' : 'B');
        }
        permission_snapshot_swap(current, snapshot);
        cache->active_slot = target_slot;
        cache->has_snapshot = 1;
    }
    return status;
}

static pxa_status_t legacy_nvs_load(
    pxa_bytes_t identity, pxa_bytes_t name, pxa_bytes_t scope,
    pxa_permission_decision_t *decision) {
    char key[PXA_ESP_PERMISSION_LEGACY_KEY_LEN + 1];
    nvs_handle_t handle;
    uint8_t value;
    size_t size = 0;
    esp_err_t status;
    if (g_legacy_namespace_absent) return PXA_STATUS_NOT_FOUND;
    if (!derive_legacy_key(identity, name, scope, key)) {
        return PXA_STATUS_INTERNAL;
    }
    status = nvs_open(PXA_ESP_PERMISSION_LEGACY_NS, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        g_legacy_namespace_absent = 1;
        return PXA_STATUS_NOT_FOUND;
    }
    if (status != ESP_OK) return PXA_STATUS_INTERNAL;
    status = nvs_get_blob(handle, key, NULL, &size);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return PXA_STATUS_NOT_FOUND;
    }
    if (status != ESP_OK || size != sizeof(value)) {
        nvs_close(handle);
        return PXA_STATUS_INTERNAL;
    }
    size = sizeof(value);
    status = nvs_get_blob(handle, key, &value, &size);
    nvs_close(handle);
    if (status != ESP_OK ||
        (value != PXA_PERMISSION_DENY && value != PXA_PERMISSION_ALLOW)) {
        return PXA_STATUS_INTERNAL;
    }
    *decision = value;
    return PXA_STATUS_OK;
}

static int legacy_nvs_erase(pxa_bytes_t identity, pxa_bytes_t name,
                            pxa_bytes_t scope) {
    char key[PXA_ESP_PERMISSION_LEGACY_KEY_LEN + 1];
    nvs_handle_t handle;
    esp_err_t status;
    if (!derive_legacy_key(identity, name, scope, key) ||
        nvs_open(PXA_ESP_PERMISSION_LEGACY_NS, NVS_READWRITE, &handle) !=
            ESP_OK) {
        return 0;
    }
    status = nvs_erase_key(handle, key);
    if (status == ESP_ERR_NVS_NOT_FOUND) status = ESP_OK;
    if (status == ESP_OK) status = nvs_commit(handle);
    nvs_close(handle);
    return status == ESP_OK;
}

static int valid_arguments(pxa_bytes_t identity, pxa_bytes_t name,
                           pxa_bytes_t scope) {
    return identity.data != NULL && identity.size != 0 &&
           name.data != NULL && name.size != 0 &&
           (scope.data != NULL || scope.size == 0) &&
           identity.size <= UINT32_MAX && name.size <= UINT32_MAX &&
           scope.size <= UINT32_MAX;
}

pxa_status_t pxa_esp_permission_store_load(
    void *context, pxa_bytes_t app_identity, pxa_bytes_t name,
    pxa_bytes_t scope, pxa_permission_decision_t *decision) {
    pxa_esp_permission_cache_t *cache;
    pxa_permission_decision_t legacy_decision = PXA_PERMISSION_DENY;
    pxa_status_t status;
    pxa_status_t cache_status;
    pxa_status_t legacy_status;
    (void)context;
    if (decision == NULL || !valid_arguments(app_identity, name, scope)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&g_permission_store_mutex);
    status = policy_load(app_identity, name, scope, decision);
    if (status != PXA_STATUS_NOT_FOUND) {
        pthread_mutex_unlock(&g_permission_store_mutex);
        return status;
    }
    cache_status = select_permission_cache(app_identity, &cache);
    if (cache_status != PXA_STATUS_OK) {
        pthread_mutex_unlock(&g_permission_store_mutex);
        return cache_status;
    }
    if (permission_cache_legacy_checked(cache, name, scope)) {
        pthread_mutex_unlock(&g_permission_store_mutex);
        return PXA_STATUS_NOT_FOUND;
    }
    legacy_status = legacy_nvs_load(app_identity, name, scope,
                                    &legacy_decision);
    if (legacy_status == PXA_STATUS_OK) {
        if (legacy_decision == PXA_PERMISSION_ALLOW) {
            if (policy_save(app_identity, name, scope, legacy_decision) ==
                PXA_STATUS_OK) {
                permission_cache_unmark_legacy_checked(cache, name, scope);
                (void)legacy_nvs_erase(app_identity, name, scope);
                ESP_LOGI(PXA_ESP_PERMISSION_TAG,
                         "Migrated one legacy NVS permission grant");
            }
        } else {
            (void)legacy_nvs_erase(app_identity, name, scope);
            permission_cache_mark_legacy_checked(cache, name, scope);
        }
        *decision = legacy_decision;
        pthread_mutex_unlock(&g_permission_store_mutex);
        return PXA_STATUS_OK;
    }
    if (legacy_status == PXA_STATUS_NOT_FOUND) {
        permission_cache_mark_legacy_checked(cache, name, scope);
    }
    pthread_mutex_unlock(&g_permission_store_mutex);
    return legacy_status;
}

pxa_status_t pxa_esp_permission_store_save(
    void *context, pxa_bytes_t app_identity, pxa_bytes_t name,
    pxa_bytes_t scope, pxa_permission_decision_t decision) {
    pxa_esp_permission_cache_t *cache = NULL;
    pxa_permission_decision_t legacy_decision = PXA_PERMISSION_DENY;
    pxa_status_t legacy_status;
    pxa_status_t status;
    (void)context;
    if (!valid_arguments(app_identity, name, scope) ||
        (decision != PXA_PERMISSION_DENY &&
         decision != PXA_PERMISSION_ALLOW)) {
        return PXA_STATUS_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&g_permission_store_mutex);
    legacy_status = legacy_nvs_load(app_identity, name, scope,
                                    &legacy_decision);
    if (decision == PXA_PERMISSION_DENY &&
        legacy_status == PXA_STATUS_OK &&
        !legacy_nvs_erase(app_identity, name, scope)) {
        pthread_mutex_unlock(&g_permission_store_mutex);
        return PXA_STATUS_INTERNAL;
    }
    status = policy_save(app_identity, name, scope, decision);
    if (status == PXA_STATUS_OK &&
        select_permission_cache(app_identity, &cache) == PXA_STATUS_OK) {
        if (decision == PXA_PERMISSION_ALLOW) {
            permission_cache_unmark_legacy_checked(cache, name, scope);
            if (legacy_status == PXA_STATUS_OK) {
                (void)legacy_nvs_erase(app_identity, name, scope);
            }
        } else if (legacy_status == PXA_STATUS_OK ||
                   legacy_status == PXA_STATUS_NOT_FOUND) {
            permission_cache_mark_legacy_checked(cache, name, scope);
        }
    }
    pthread_mutex_unlock(&g_permission_store_mutex);
    return status;
}

pxa_status_t pxa_esp_permission_store_clear_app(
    pxa_bytes_t app_identity, const pxa_package_permission_t *permissions,
    size_t permission_count) {
    char path[PXA_ESP_PERMISSION_PATH_BYTES];
    pxa_status_t status = PXA_STATUS_OK;
    size_t index;
    int slot;
    if (app_identity.data == NULL || app_identity.size == 0 ||
        (permission_count != 0 && permissions == NULL))
        return PXA_STATUS_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_permission_store_mutex);
    for (slot = 0; slot < 2; ++slot) {
        if (!build_slot_path(app_identity, slot, path, sizeof(path))) {
            status = PXA_STATUS_INTERNAL;
            break;
        }
        if (unlink(path) != 0 && errno != ENOENT) status = PXA_STATUS_IO_ERROR;
    }
    for (index = 0; index < permission_count; ++index) {
        (void)legacy_nvs_erase(app_identity, permissions[index].name,
                               permissions[index].scope);
    }
    if (g_permission_cache != NULL) permission_cache_clear(g_permission_cache);
    pthread_mutex_unlock(&g_permission_store_mutex);
    return status;
}

void pxa_esp_permission_store_bind(pxa_permission_store_t *store) {
    if (store == NULL) return;
    memset(store, 0, sizeof(*store));
    store->struct_size = sizeof(*store);
    store->context = NULL;
    store->load = pxa_esp_permission_store_load;
    store->save = pxa_esp_permission_store_save;
}

#endif /* ESP_PLATFORM */
