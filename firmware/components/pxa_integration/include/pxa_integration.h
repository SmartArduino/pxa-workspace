#ifndef PXA_INTEGRATION_H
#define PXA_INTEGRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pxsys/theme.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t struct_size;
    const char* locale;
    const char* font_path;
    pxsys_color_scheme_t display_scheme;
    size_t max_apps;
    size_t max_instances;
    size_t max_tasks;
    bool mount_storage;
} pxa_product_profile_t;

void pxa_product_profile_init(pxa_product_profile_t* profile);
bool pxa_integration_start(const pxa_product_profile_t* profile);
void pxa_integration_stop(void);

#ifdef __cplusplus
}
#endif

#endif
