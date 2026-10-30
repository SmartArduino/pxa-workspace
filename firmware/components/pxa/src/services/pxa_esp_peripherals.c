#include "pxa/pxa_peripherals.h"
#include <string.h>
static pxa_peripherals_backend_t backend;
bool pxa_esp_peripherals_set_backend(const pxa_peripherals_backend_t *value) {
    if (backend.struct_size || !value || value->struct_size < PXA_PERIPHERALS_BACKEND_BASE_SIZE ||
        !value->list || !value->module || !value->open || !value->io || !value->start ||
        !value->poll || !value->cancel || !value->close) return false;
    memcpy(&backend, value, value->struct_size >= sizeof(*value) ?
        sizeof(*value) : PXA_PERIPHERALS_BACKEND_BASE_SIZE); return true;
}
const pxa_peripherals_backend_t *pxa_esp_peripherals_backend(void) {
    return backend.struct_size ? &backend : NULL;
}
