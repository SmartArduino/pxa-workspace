/* Exercise the FAT-compatible path with no LittleFS F_GETPATH support. */
#include "../../../../deps/pxa-system/libpxa/adapters/esp/pxa_esp_posix_shim.c"
#include <assert.h>

static char root[PXA_ESP_POSIX_PATH_MAX];
static uint64_t epoch = 10;
static unsigned sync_count, allocations;
static int fail_allocation, fail_sync;

void *heap_caps_malloc(size_t size, unsigned caps) {
    void *memory;
    (void)caps;
    if (fail_allocation) return NULL;
    memory = malloc(size);
    if (memory) ++allocations;
    return memory;
}
void heap_caps_free(void *memory) { if (memory) { assert(allocations); --allocations; } free(memory); }

static uint64_t mount_epoch(void *context, const char *path) {
    const char *relative;
    (void)context;
    if (!path_contains(root, path)) return 0;
    relative = path + strlen(root);
    if (!strncmp(relative, "/mount", 6) && (!relative[6] || relative[6] == '/')) return 20;
    return epoch;
}
static int sync_directory(void *context, const char *path, uint64_t selected_epoch) {
    (void)context;
    assert(path_contains(root, path) && selected_epoch == epoch);
    ++sync_count;
    if (fail_sync) { errno = EIO; return -1; }
    return 0;
}

int main(void) {
    pxa_esp_posix_directory_hooks_t hooks = {NULL, mount_epoch, sync_directory};
    char normalized[PXA_ESP_POSIX_PATH_MAX], moved[PXA_ESP_POSIX_PATH_MAX], data[4] = {0};
    struct stat metadata;
    int directory, child, duplicate, file, stale, handles[PXA_ESP_POSIX_DIRECTORY_HANDLES];
    unsigned count;
    DIR *stream;
    const char *temporary = getenv("TMPDIR");
    assert(snprintf(root, sizeof(root), "%s/pxa-directory-XXXXXX", temporary ? temporary : "/tmp") < (int)sizeof(root));
    assert(mkdtemp(root));
    assert(pxa_esp_posix_set_directory_hooks(&hooks) == 0);
    assert(snprintf(normalized, sizeof(normalized), "%s//./", root) < (int)sizeof(normalized));
    directory = pxa_esp_open(normalized, O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    assert(pxa_esp_posix_set_directory_hooks(NULL) == -1 && errno == EBUSY);
    assert(pxa_esp_fstat(directory, &metadata) == 0 && S_ISDIR(metadata.st_mode));
    assert(pxa_esp_mkdirat(directory, "original", 0700) == 0);
    /* The nested filesystem is mounted by its owner, outside the parent
     * capability. A parent directory cannot create or traverse into it. */
    assert(snprintf(moved, sizeof(moved), "%s/mount", root) < (int)sizeof(moved));
    assert(mkdir(moved, 0700) == 0);
    assert(pxa_esp_openat(directory, "./mount", O_RDONLY | O_DIRECTORY) == -1 && errno == EXDEV);
    assert(pxa_esp_opendirat(directory, "./mount") == NULL && errno == EXDEV);
    assert(pxa_esp_opendirat(directory, "original/../mount") == NULL && errno == EINVAL);
    assert(pxa_esp_opendirat(directory, "/") == NULL && errno == EINVAL);
    assert(pxa_esp_openat(directory, "original/../mount", O_RDONLY | O_DIRECTORY) == -1 && errno == EINVAL);
    child = pxa_esp_openat(directory, "original", O_RDONLY | O_DIRECTORY);
    assert(child >= 0);
    file = pxa_esp_openat(child, "data", O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(file >= 0 && write(file, "abc", 3) == 3 && pxa_esp_fsync(file) == 0);
    assert(pxa_esp_close(file) == 0);
    stream = pxa_esp_opendirat(directory, "original");
    assert(stream != NULL);
    int found_data = 0;
    struct dirent *item;
    while ((item = readdir(stream)) != NULL)
        if (strcmp(item->d_name, "data") == 0) found_data = 1;
    assert(found_data && closedir(stream) == 0);
    duplicate = pxa_esp_dup(child);
    assert(duplicate >= 0);
    /* Allocate all replacement paths before mutation. A failed update must
     * leave both directory references and the on-disk engine untouched. */
    fail_allocation = 1;
    assert(pxa_esp_renameat(directory, "original", directory, "moved") == -1 && errno == ENOMEM);
    fail_allocation = 0;
    assert(pxa_esp_fstatat(directory, "original/data", &metadata, AT_SYMLINK_NOFOLLOW) == 0);
    assert(pxa_esp_renameat(directory, "original", directory, "moved") == 0);
    assert(pxa_esp_fstat(duplicate, &metadata) == 0 && S_ISDIR(metadata.st_mode));
    file = pxa_esp_openat(child, "data", O_RDONLY);
    assert(file >= 0 && read(file, data, 3) == 3 && !strcmp(data, "abc"));
    assert(pxa_esp_close(file) == 0);
    assert(pxa_esp_mkdirat(directory, "replacement", 0700) == 0);
    file = pxa_esp_openat(directory, "replacement", O_RDONLY | O_DIRECTORY);
    assert(file >= 0);
    assert(pxa_esp_renameat(directory, "moved", directory, "replacement") == -1 && errno == EBUSY);
    assert(pxa_esp_close(file) == 0);
    assert(pxa_esp_unlinkat(directory, "replacement", AT_REMOVEDIR) == 0);
    assert(pxa_esp_renameat(directory, "moved", directory, "mount/engine") == -1 && errno == EXDEV);
    assert(pxa_esp_fsync(child) == 0 && sync_count == 1);
    fail_sync = 1;
    assert(pxa_esp_fsync(child) == -1 && errno == EIO && sync_count == 2);
    fail_sync = 0;
    stream = pxa_esp_fdopendir(duplicate);
    assert(stream && readdir(stream));
    assert(pxa_esp_close(duplicate) == -1 && errno == EBADF);
    assert(closedir(stream) == 0);
    assert(pxa_esp_unlinkat(child, "data", 0) == 0);
    assert(pxa_esp_unlinkat(directory, "moved", AT_REMOVEDIR) == 0);
    assert(pxa_esp_mkdirat(directory, "moved", 0700) == 0);
    assert(pxa_esp_fstat(child, &metadata) == -1 && errno == EBADF);
    assert(pxa_esp_close(child) == 0);
    for (count = 0; count < PXA_ESP_POSIX_DIRECTORY_HANDLES - 1; ++count) {
        handles[count] = pxa_esp_dup(directory); assert(handles[count] >= 0);
    }
    assert(pxa_esp_dup(directory) == -1 && errno == EMFILE);
    stale = handles[0]; assert(pxa_esp_close(stale) == 0);
    handles[0] = pxa_esp_dup(directory); assert(handles[0] >= 0 && handles[0] != stale);
    assert(pxa_esp_close(stale) == -1 && errno == EBADF);
    for (count = 0; count < PXA_ESP_POSIX_DIRECTORY_HANDLES - 1; ++count) assert(pxa_esp_close(handles[count]) == 0);
    epoch = UINT64_MAX;
    assert(pxa_esp_fstat(directory, &metadata) == -1 && errno == EIO);
    epoch = 11;
    assert(pxa_esp_openat(directory, "moved", O_RDONLY | O_DIRECTORY) == -1 && errno == EIO);
    assert(pxa_esp_close(directory) == 0);
    assert(pxa_esp_posix_set_directory_hooks(NULL) == 0);
    assert(snprintf(moved, sizeof(moved), "%s/moved", root) < (int)sizeof(moved));
    assert(pxa_esp_rmdir(moved) == 0);
    assert(snprintf(moved, sizeof(moved), "%s/mount", root) < (int)sizeof(moved));
    assert(pxa_esp_rmdir(moved) == 0);
    assert(pxa_esp_rmdir(root) == 0 && allocations == 0);
    puts("ESP directory handles: rename, sync, exhaustion and mount epochs passed");
    return 0;
}
