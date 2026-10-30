#include "pxa/pxa_esp_haptics.h"
static pxa_haptics_backend_t backend;
bool pxa_esp_haptics_set_backend(const pxa_haptics_backend_t *value) {
    if (backend.struct_size || !value || value->struct_size < sizeof(*value) ||
        !value->play || !value->stop || !value->max_duration_ms ||
        !(value->flags & PXA_HAPTICS_CAP_VIBRATE)) return false;
    backend = *value;
    return true;
}
const pxa_haptics_backend_t *pxa_esp_haptics_backend(void) {
    return backend.struct_size ? &backend : NULL;
}
