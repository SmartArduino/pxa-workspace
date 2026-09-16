#ifndef PXA_ESP_PACKAGE_ICON_H
#define PXA_ESP_PACKAGE_ICON_H

#include "pxa/pxa_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An empty icon tells the selected system UI to use its own fallback. */
pxa_host_icon_t pxa_esp_package_icon_default(void);

/* Load one signed Package PNG, returning an empty icon on error. */
pxa_host_icon_t pxa_esp_package_icon_load(const char *root,
                                          const char *relative_path);

#ifdef __cplusplus
}
#endif

#endif
