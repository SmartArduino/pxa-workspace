// SPDX-License-Identifier: Apache-2.0
#include "pxa/pxa_esp_video.h"
#include <string.h>
static pxa_video_backend_t backend;
bool pxa_esp_video_set_backend(const pxa_video_backend_t *value) {
    if (backend.struct_size || !value) return false;
    const size_t bytes = pxa_video_backend_copy_size(value->struct_size);
    if (!bytes || !value->count || !value->capabilities || !value->open || !value->decode ||
        !value->reset || !value->close) return false;
    memcpy(&backend, value, bytes);
    backend.struct_size = (uint32_t)bytes;
    return true;
}
const pxa_video_backend_t *pxa_esp_video_backend(void) {
    return backend.struct_size ? &backend : NULL;
}
