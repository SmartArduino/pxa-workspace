#define open test_open
#define fstat test_fstat
#define fsync test_fsync
#include "../../../../deps/pxa-system/libpxa/adapters/esp/pxa_esp_posix_shim.c"
#undef open
#undef fstat
#undef fsync
#include <assert.h>

static unsigned opens, syncs;
static mode_t stat_mode;
int test_open(const char *path, int flags, ...) {
    assert(!strcmp(path, "/dev/null") && flags == O_RDWR);
    if (++opens == 1) {errno = EMFILE; return -1;}
    return 123;
}
int test_fstat(int fd, struct stat *metadata) {
    assert(fd == 123);
    if (stat_mode == 0) { errno = ENOENT; return -1; }
    memset(metadata, 0, sizeof(*metadata));
    metadata->st_mode = stat_mode;
    return 0;
}
int test_fsync(int fd) { assert(fd == 123); ++syncs; return 0; }
int main(void) {
    assert(pxa_esp_wasi_null_fd() == -1); // A failed open may be retried.
    for (int i = 0; i < 64; ++i) assert(pxa_esp_wasi_null_fd() == 123);
    assert(opens == 2);
    /* A removed directory's failed stat must never reach the file-only
     * LittleFS fsync callback (which would assert on that handle). */
    assert(pxa_esp_fsync(123) == -1 && errno == ENOENT && syncs == 0);
    stat_mode = S_IFDIR;
    assert(pxa_esp_fsync(123) == 0 && syncs == 0);
    stat_mode = S_IFREG;
    assert(pxa_esp_fsync(123) == 0 && syncs == 1);
}
