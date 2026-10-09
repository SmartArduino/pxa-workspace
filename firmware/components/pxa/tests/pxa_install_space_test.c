#include "pxa/esp/pxa_esp_install_space.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    pxa_package_file_t files[2] = {0};
    pxa_package_manifest_t manifest = {0};
    manifest.files = files;
    manifest.file_count = 2;
    files[0].size = 574044; /* Actual S3 code, not its compressed upload size. */
    files[0].path = (pxa_bytes_t){(const uint8_t *)"artifacts/main.aot", 18};
    files[1].size = 39204;
    files[1].path = (pxa_bytes_t){(const uint8_t *)"assets/font.raw", 15};
    uint64_t required = pxa_esp_install_space_required(&manifest, 4096);
    assert(required > files[0].size + files[1].size + 128 * 1024);
    assert(!pxa_esp_install_space_available(required, 9 * 1024 * 1024,
                                           9 * 1024 * 1024));
    assert(pxa_esp_install_space_available(required, (size_t)required, 0));
    assert(!pxa_esp_install_space_available(required, (size_t)required - 1, 0));
    assert(!pxa_esp_install_space_available(required, 100, 101));
    files[0].size = UINT64_MAX;
    assert(pxa_esp_install_space_required(&manifest, 4096) == UINT64_MAX);
    assert(!pxa_esp_install_space_available(UINT64_MAX, SIZE_MAX, 0));
    assert(pxa_esp_install_space_required(NULL, 0) == UINT64_MAX);
    assert(pxa_esp_install_space_required(&manifest, SIZE_MAX) == UINT64_MAX);
    files[0].size = 0;
    uint64_t with_parent = pxa_esp_install_space_required(&manifest, 1);
    files[0].path = (pxa_bytes_t){(const uint8_t *)"main.aot", 8};
    assert(with_parent - pxa_esp_install_space_required(&manifest, 1) == 8192);
    files[0].path = (pxa_bytes_t){(const uint8_t *)"assets/other.raw", 16};
    uint64_t shared_parent = pxa_esp_install_space_required(&manifest, 1);
    assert(shared_parent == with_parent - 8192);
    files[0].path = (pxa_bytes_t){(const uint8_t *)"assets/sub/other.raw", 20};
    assert(pxa_esp_install_space_required(&manifest, 1) == shared_parent + 8192);
    puts("Install admission: decoded bytes, metadata, cleanup reserve and overflow passed.");
}
