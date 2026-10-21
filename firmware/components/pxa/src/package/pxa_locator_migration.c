#include "pxa_locator_migration.h"
pxa_status_t pxa_locator_migrate(pxa_app_locators_t *r, const uint8_t root[32], const char *app_id,
                                 uint16_t bit, const char *old_path, const char *new_path,
                                 const pxa_locator_migration_io_t *io) {
    char relative[130];
    uint16_t flags = 0;
    if (!io || !io->probe || !io->move || !old_path || !new_path ||
        (bit != 1 && bit != 2 && bit != 4))
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status =
        pxa_app_locators_find(r, root, app_id, &flags, relative, sizeof(relative));
    if (status != PXA_STATUS_OK || !(flags & bit))
        return status;
    int old = io->probe(io->context, old_path), next = io->probe(io->context, new_path);
    if (old < 0 || next < 0)
        return PXA_STATUS_IO_ERROR;
    if (old && next)
        return PXA_STATUS_DENIED;
    uint16_t moving = (uint16_t)(bit << 3);
    if (!(flags & moving)) {
        if (next)
            return PXA_STATUS_DENIED; /* unclaimed destination */
        if (!old)
            return pxa_app_locators_set_flags(r, root, app_id, (uint16_t)(flags & ~bit));
        status = pxa_app_locators_set_flags(r, root, app_id, (uint16_t)(flags | moving));
        if (status != PXA_STATUS_OK)
            return status;
        flags |= moving;
    } else if (!old && !next)
        return PXA_STATUS_BAD_STATE;
    if (old) {
        status = io->move(io->context, old_path, new_path);
        if (status != PXA_STATUS_OK)
            return status;
    }
    return pxa_app_locators_set_flags(r, root, app_id, (uint16_t)(flags & ~(bit | moving)));
}
