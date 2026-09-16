#ifndef PXA_ESP_PACKAGE_CATALOG_H
#define PXA_ESP_PACKAGE_CATALOG_H

#include <stddef.h>

#include "app_pages.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Product-UI projections over the neutral signed Package repository. */
size_t pxa_esp_package_catalog_apps(app_pages_app_t *apps, size_t capacity);
size_t pxa_esp_package_catalog_managed(app_pages_managed_app_t *apps,
                                       size_t capacity);
size_t pxa_esp_package_catalog_permissions(
    const char *identity, app_pages_app_permission_t *permissions,
    size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
