/* Exercise nested VFS callbacks like private LittleFS over sparse FAT files. */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>

static int backend_rename(const char *, const char *);
static int backend_rmdir(const char *);
static DIR *backend_opendir(const char *);
#define rename backend_rename
#define rmdir backend_rmdir
#define opendir backend_opendir
#include "../../../../deps/pxa-system/libpxa/adapters/esp/pxa_esp_posix_shim.c"
#undef rename
#undef rmdir
#undef opendir

static char root[PXA_ESP_POSIX_PATH_MAX], guest[PXA_ESP_POSIX_PATH_MAX];
static char moving[PXA_ESP_POSIX_PATH_MAX], moved[PXA_ESP_POSIX_PATH_MAX];
static char opening[PXA_ESP_POSIX_PATH_MAX];
static int guest_fd, backing_fd, reference, reused;
static unsigned allocations, nested_renames, nested_removes;
static int fail_rename, fail_rmdir, fail_open;
static void (*during_mutation)(void);
static void (*during_open)(void);

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    (void)caps;
    void *memory = malloc(bytes);
    if (memory) __sync_fetch_and_add(&allocations, 1);
    return memory;
}
void heap_caps_free(void *memory) {
    if (memory) { assert(__sync_fetch_and_sub(&allocations, 1)); free(memory); }
}

static int backend_rename(const char *source, const char *target) {
    if (path_contains(guest, source)) {
        if (during_mutation) during_mutation();
        if (fail_rename) { errno = EIO; return -1; }
    } else ++nested_renames;
    return rename(source, target);
}
static int backend_rmdir(const char *path) {
    if (path_contains(guest, path)) {
        if (during_mutation) during_mutation();
        if (fail_rmdir) { errno = EIO; return -1; }
    } else ++nested_removes;
    return rmdir(path);
}
static DIR *backend_opendir(const char *path) {
    if (opening[0] && !strcmp(path, opening)) {
        if (during_open) during_open();
        if (fail_open) { errno = EIO; return NULL; }
    }
    return opendir(path);
}

static void write_at(int directory, const char *name, const char *value) {
    int fd = pxa_esp_openat(directory, name, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value));
    assert(pxa_esp_close(fd) == 0);
}
static void expect_at(int directory, const char *name, const char *expected) {
    char bytes[32] = {0};
    int fd = pxa_esp_openat(directory, name, O_RDONLY);
    assert(fd >= 0 && read(fd, bytes, sizeof(bytes) - 1) == (ssize_t)strlen(expected));
    assert(!strcmp(bytes, expected) && pxa_esp_close(fd) == 0);
}

static void backing_io(void) {
    /* Reads, extent allocation/rename and cleanup all reenter the adapter. */
    expect_at(backing_fd, "block", "extent");
    write_at(backing_fd, "pending", "extent");
    assert(pxa_esp_renameat(backing_fd, "pending", backing_fd, "block") == 0);
    assert(pxa_esp_mkdirat(backing_fd, "unused", 0700) == 0);
    assert(pxa_esp_unlinkat(backing_fd, "unused", AT_REMOVEDIR) == 0);
}

static void *concurrent_references(void *unused) {
    struct stat st;
    (void)unused;
    /* Overlapping operations fail promptly; neither thread waits on VFS. */
    assert(pxa_esp_fstat(reference, &st) == -1 && errno == EBUSY);
    assert(pxa_esp_open(moving, O_RDONLY | O_DIRECTORY) == -1 && errno == EBUSY);
    assert(pxa_esp_open(moved, O_RDONLY | O_DIRECTORY) == -1 && errno == EBUSY);
    assert(pxa_esp_rename(moving, moved) == -1 && errno == EBUSY);
    assert(pxa_esp_rmdir(moving) == -1 && errno == EBUSY);
    assert(pxa_esp_posix_set_directory_hooks(NULL) == -1 && errno == EBUSY);
    /* Closing and reusing the same table slot must not inherit the new path. */
    assert(pxa_esp_close(reference) == 0);
    reused = pxa_esp_dup(backing_fd);
    assert(reused >= 0 && reused != reference);
    assert((reused & 31) == (reference & 31));
    expect_at(reused, "block", "extent");
    return NULL;
}
static void concurrent_io(void) {
    pthread_t worker;
    backing_io();
    assert(pthread_create(&worker, NULL, concurrent_references, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
}

static void opening_conflicts(void) {
    /* The filesystem has not returned the new directory yet. Do not let a
     * rename/removal publish a stale pathname reference afterwards. */
    assert(pxa_esp_rename(opening, moved) == -1 && errno == EBUSY);
    assert(pxa_esp_rmdir(opening) == -1 && errno == EBUSY);
    assert(pxa_esp_posix_set_directory_hooks(NULL) == -1 && errno == EBUSY);
    backing_io();
}

int main(void) {
    const char *temporary = getenv("TMPDIR");
    char path[PXA_ESP_POSIX_PATH_MAX];
    struct stat st;
    /* A regression must fail in bounded time, including deadlocked workers. */
    alarm(15);
    assert(snprintf(root, sizeof(root), "%s/pxa-nested-directory-XXXXXX",
                    temporary ? temporary : "/tmp") < (int)sizeof(root));
    assert(mkdtemp(root));
    assert(snprintf(guest, sizeof(guest), "%s/private", root) < (int)sizeof(guest));
    assert(snprintf(path, sizeof(path), "%s/backing", root) < (int)sizeof(path));
    assert(mkdir(guest, 0700) == 0 && mkdir(path, 0700) == 0);
    guest_fd = pxa_esp_open(guest, O_RDONLY | O_DIRECTORY);
    backing_fd = pxa_esp_open(path, O_RDONLY | O_DIRECTORY);
    assert(guest_fd >= 0 && backing_fd >= 0);
    write_at(backing_fd, "block", "extent");

    write_at(guest_fd, "staging", "new save");
    write_at(guest_fd, "save", "old save");
    during_mutation = backing_io;
    fail_rename = 1;
    assert(pxa_esp_renameat(guest_fd, "staging", guest_fd, "save") == -1 && errno == EIO);
    expect_at(guest_fd, "save", "old save");
    expect_at(guest_fd, "staging", "new save");
    fail_rename = 0;
    assert(pxa_esp_renameat(guest_fd, "staging", guest_fd, "save") == 0);
    expect_at(guest_fd, "save", "new save");
    assert(pxa_esp_fstatat(guest_fd, "staging", &st, 0) == -1 && errno == ENOENT);

    assert(pxa_esp_mkdirat(guest_fd, "moving", 0700) == 0);
    assert(snprintf(moving, sizeof(moving), "%s/moving", guest) < (int)sizeof(moving));
    assert(snprintf(moved, sizeof(moved), "%s/moved", guest) < (int)sizeof(moved));
    reference = pxa_esp_openat(guest_fd, "moving", O_RDONLY | O_DIRECTORY);
    int retained = pxa_esp_dup(reference);
    assert(reference >= 0 && retained >= 0);
    write_at(reference, "child", "directory data");
    fail_rename = 1;
    assert(pxa_esp_rename(moving, moved) == -1 && errno == EIO);
    expect_at(reference, "child", "directory data");
    assert(pxa_esp_fstatat(guest_fd, "moved", &st, 0) == -1 && errno == ENOENT);
    fail_rename = 0;
    during_mutation = concurrent_io;
    assert(pxa_esp_rename(moving, moved) == 0);
    during_mutation = backing_io;
    expect_at(retained, "child", "directory data");
    expect_at(reused, "block", "extent");
    assert(pxa_esp_fstat(reference, &st) == -1 && errno == EBADF);
    assert(pxa_esp_close(reused) == 0);

    assert(pxa_esp_unlinkat(retained, "child", 0) == 0);
    fail_rmdir = 1;
    assert(pxa_esp_rmdir(moved) == -1 && errno == EIO);
    assert(pxa_esp_fstat(retained, &st) == 0 && S_ISDIR(st.st_mode));
    fail_rmdir = 0;
    assert(pxa_esp_rmdir(moved) == 0);
    assert(mkdir(moved, 0700) == 0);
    assert(pxa_esp_fstat(retained, &st) == -1 && errno == EBADF);
    assert(pxa_esp_close(retained) == 0);
    assert(pxa_esp_rmdir(moved) == 0);

    assert(snprintf(opening, sizeof(opening), "%s/opening", guest) < (int)sizeof(opening));
    assert(mkdir(opening, 0700) == 0);
    during_open = opening_conflicts;
    fail_open = 1;
    assert(pxa_esp_open(opening, O_RDONLY | O_DIRECTORY) == -1 && errno == EIO);
    fail_open = 0;
    int opened = pxa_esp_open(opening, O_RDONLY | O_DIRECTORY);
    assert(opened >= 0);
    during_open = NULL;
    assert(pxa_esp_rename(opening, moved) == 0);
    assert(pxa_esp_fstat(opened, &st) == 0 && S_ISDIR(st.st_mode));
    assert(pxa_esp_close(opened) == 0 && pxa_esp_rmdir(moved) == 0);
    assert(nested_renames >= 10 && nested_removes >= 10);

    during_mutation = NULL;
    assert(pxa_esp_unlinkat(guest_fd, "save", 0) == 0);
    assert(pxa_esp_unlinkat(backing_fd, "block", 0) == 0);
    assert(pxa_esp_close(guest_fd) == 0 && pxa_esp_close(backing_fd) == 0);
    assert(pxa_esp_posix_set_directory_hooks(NULL) == 0);
    assert(rmdir(guest) == 0 && rmdir(path) == 0 && rmdir(root) == 0);
    assert(allocations == 0 && g_directory_operations == NULL);
    alarm(0);
    puts("Nested VFS: openat/renameat/rmdir, failed replacement, concurrent references and slot reuse passed");
    return 0;
}
