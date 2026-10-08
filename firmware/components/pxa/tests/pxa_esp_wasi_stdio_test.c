#define open test_open
#include "../../../../deps/pxa-system/libpxa/adapters/esp/pxa_esp_posix_shim.c"
#undef open
#include <assert.h>

static unsigned opens;
int test_open(const char *path, int flags, ...) {
    assert(!strcmp(path, "/dev/null") && flags == O_RDWR);
    if (++opens == 1) {errno = EMFILE; return -1;}
    return 123;
}
int main(void) {
    assert(pxa_esp_wasi_null_fd() == -1); // A failed open may be retried.
    for (int i = 0; i < 64; ++i) assert(pxa_esp_wasi_null_fd() == 123);
    assert(opens == 2);
}
