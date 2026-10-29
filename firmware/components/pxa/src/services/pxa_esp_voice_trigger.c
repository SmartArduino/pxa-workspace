// SPDX-License-Identifier: Apache-2.0
#include <pxa/pxa_voice_trigger.h>

static pxa_vt_backend_t backend;

bool pxa_esp_voice_trigger_set_backend(const pxa_vt_backend_t *value) {
    if (backend.struct_size || !value || value->struct_size < sizeof(*value) ||
        !value->profile || !value->start || !value->read || !value->stop)
        return false;
    backend = *value;
    return true;
}
const pxa_vt_backend_t *pxa_esp_voice_trigger_backend(void) {
    return backend.struct_size ? &backend : NULL;
}
