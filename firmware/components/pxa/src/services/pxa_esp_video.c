// SPDX-License-Identifier: Apache-2.0
#include "pxa/pxa_esp_video.h"
static pxa_video_backend_t backend;
bool pxa_esp_video_set_backend(const pxa_video_backend_t *value) {
    if (backend.struct_size || !value || value->struct_size < sizeof(*value) ||
        !value->count || !value->capabilities || !value->open || !value->decode ||
        !value->reset || !value->close) return false;
    backend = *value; return true;
}
const pxa_video_backend_t *pxa_esp_video_backend(void) {
    return backend.struct_size ? &backend : NULL;
}
