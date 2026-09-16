#ifndef PXSYS_ESP_PXA_BRIDGE_H
#define PXSYS_ESP_PXA_BRIDGE_H

#include "pxa/pxa_host.h"
#include "pxsys/app_metadata.h"
#include "pxsys/standard_system.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t struct_size;
    size_t max_packages;
    size_t max_subscriptions;
    size_t max_exported_services;
    size_t max_pending_calls;
    pxsys_standard_system_t* system;
    pxsys_allocator_t allocator;
    void* catalog_synced_context;
    void (*catalog_synced)(void* context);
} pxsys_esp_pxa_bridge_config_t;

typedef struct pxsys_esp_pxa_bridge pxsys_esp_pxa_bridge_t;

void pxsys_esp_pxa_bridge_config_init(pxsys_esp_pxa_bridge_config_t* config);
pxsys_status_t pxsys_esp_pxa_bridge_create(const pxsys_esp_pxa_bridge_config_t* config,
                                           pxsys_esp_pxa_bridge_t** output);
pxsys_status_t pxsys_esp_pxa_bridge_destroy(pxsys_esp_pxa_bridge_t* bridge);
pxsys_status_t pxsys_esp_pxa_bridge_sync(pxsys_esp_pxa_bridge_t* bridge);
bool pxsys_esp_pxa_bridge_resolve_app_metadata(
    pxsys_esp_pxa_bridge_t* bridge, const pxsys_app_descriptor_t* app,
    const pxsys_locale_snapshot_t* locale, pxsys_app_metadata_t* metadata);
bool pxsys_esp_pxa_bridge_resolve_app_icon(
    pxsys_esp_pxa_bridge_t* bridge, const pxsys_app_descriptor_t* app,
    pxa_host_icon_t* icon);

#ifdef __cplusplus
}
#endif

#endif
