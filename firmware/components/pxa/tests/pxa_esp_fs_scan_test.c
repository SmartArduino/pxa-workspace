/* Run the SDK's low-FD/small-stack tree fixture through the ESP shim and
 * native directory scan. Exercise filesystems with and without d_type. */
#define _GNU_SOURCE
#include "esp_heap_caps.h"
#include <dirent.h>
#include <stdlib.h>

void *heap_caps_malloc(size_t bytes, unsigned caps) {
    (void)caps;
    return malloc(bytes);
}
void heap_caps_free(void *memory) { free(memory); }

struct dirent *__real_readdir(DIR *directory);
struct dirent *__wrap_readdir(DIR *directory) {
    struct dirent *entry = __real_readdir(directory);
    if (entry && getenv("PXA_TEST_UNKNOWN_DIRENT")) entry->d_type = DT_UNKNOWN;
    return entry;
}
