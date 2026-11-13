#include "pxa_esp_permission_store.h"

#include <string.h>

pxa_status_t pxa_esp_permission_store_load(void *context, pxa_bytes_t identity,
    pxa_bytes_t name, pxa_bytes_t scope, pxa_permission_decision_t *decision) {
    (void)context; (void)identity; (void)name; (void)scope;
    if (!decision) return PXA_STATUS_INVALID_ARGUMENT;
    *decision = PXA_PERMISSION_ALLOW;
    return PXA_STATUS_OK;
}

pxa_status_t pxa_esp_permission_store_peek(void *context, pxa_bytes_t identity,
    pxa_bytes_t name, pxa_bytes_t scope, pxa_permission_decision_t *decision) {
    return pxa_esp_permission_store_load(context, identity, name, scope, decision);
}

pxa_status_t pxa_esp_permission_store_save(void *context, pxa_bytes_t identity,
    pxa_bytes_t name, pxa_bytes_t scope, pxa_permission_decision_t decision) {
    (void)context; (void)identity; (void)name; (void)scope; (void)decision;
    return PXA_STATUS_OK;
}

pxa_status_t pxa_esp_permission_store_clear_app(pxa_bytes_t identity,
    const pxa_package_permission_t *permissions, size_t count) {
    (void)identity; (void)permissions; (void)count;
    return PXA_STATUS_OK;
}

void pxa_esp_permission_store_bind(pxa_permission_store_t *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
    store->struct_size = sizeof(*store);
    store->load = pxa_esp_permission_store_load;
    store->save = pxa_esp_permission_store_save;
}

void pxa_esp_permission_store_release_cache(void) {}
