#ifndef PXA_LOCATOR_MIGRATION_H
#define PXA_LOCATOR_MIGRATION_H
#include "pxa_app_locators.h"
/* Package/private data only. Probe: 0 absent, 1 directory, negative error
 * (links/files are errors).
 * Move must never replace a destination and must persist its rename before
 * returning OK. Call only with all application file handles closed. */
typedef struct {
    void *context;
    int (*probe)(void *, const char *);
    pxa_status_t (*move)(void *, const char *, const char *);
} pxa_locator_migration_io_t;
pxa_status_t pxa_locator_migrate(pxa_app_locators_t *, const uint8_t root[32], const char *app_id,
                                 uint16_t legacy_bit, const char *old_path, const char *new_path,
                                 const pxa_locator_migration_io_t *);
#endif
