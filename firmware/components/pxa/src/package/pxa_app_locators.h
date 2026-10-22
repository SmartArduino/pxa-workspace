#ifndef PXA_APP_LOCATORS_H
#define PXA_APP_LOCATORS_H
#include <stddef.h>
#include <stdint.h>
#include "pxa/status.h"
#define PXA_LOCATOR_MAX_PUBLISHERS 16u
#define PXA_LOCATOR_MAX_APPS 32u
#define PXA_LOCATOR_PACKAGE_LEGACY 1u
#define PXA_LOCATOR_DATA_LEGACY 2u
#define PXA_LOCATOR_PACKAGE_MOVING 8u
#define PXA_LOCATOR_DATA_MOVING 16u
#define PXA_LOCATOR_ACTIVE_FLAGS (PXA_LOCATOR_PACKAGE_LEGACY | PXA_LOCATOR_DATA_LEGACY | \
                                  PXA_LOCATOR_PACKAGE_MOVING | PXA_LOCATOR_DATA_MOVING)
#define PXA_LOCATOR_SELF_NAMESPACE 1u
#define PXA_LOCATOR_VERIFIED_NAMESPACE 2u
#define PXA_LOCATOR_ENCODED_MAX                                                                    \
    (24u + 98u * PXA_LOCATOR_MAX_PUBLISHERS + 166u * PXA_LOCATOR_MAX_APPS)
/* All strings are Host locators. They never replace root+AppId authority. */
typedef struct {
    uint8_t kind, key[32];
    char directory[65];
} pxa_locator_publisher_t;
typedef struct {
    uint16_t publisher, flags;
    uint8_t root[32];
    char app_id[65], directory[65];
} pxa_locator_app_t;
typedef pxa_status_t (*pxa_locators_commit_fn)(void *, uint64_t, const pxa_locator_publisher_t *,
                                               size_t, const pxa_locator_app_t *, size_t);
typedef int (*pxa_locators_available_fn)(void *, const char *publisher, const char *app);
typedef struct {
    pxa_locator_publisher_t publishers[PXA_LOCATOR_MAX_PUBLISHERS];
    pxa_locator_app_t apps[PXA_LOCATOR_MAX_APPS];
    size_t publisher_count, app_count;
    uint64_t generation;
    void *context;
    pxa_locators_commit_fn commit;
    pxa_locators_available_fn available; /* cold allocation only, 1 means unoccupied */
    uint8_t ready;
} pxa_app_locators_t;
void pxa_app_locators_init(pxa_app_locators_t *, void *, pxa_locators_commit_fn);
pxa_status_t pxa_app_locators_load(pxa_app_locators_t *, uint64_t, const pxa_locator_publisher_t *,
                                   size_t, const pxa_locator_app_t *, size_t);
/* namespace key must come from verified Host policy: self/root or a verified
 * issuer+UUID binding. Unsigned JSON can only supply the display handle. */
pxa_status_t pxa_app_locators_reserve(pxa_app_locators_t *, uint8_t,
                                      const uint8_t namespace_key[32], const char *handle,
                                      const uint8_t root[32], const char *app_id,
                                      const uint8_t app_key[32], uint16_t legacy_flags);
pxa_status_t pxa_app_locators_find(const pxa_app_locators_t *, const uint8_t root[32],
                                   const char *app_id, uint16_t *flags, char *relative,
                                   size_t capacity);
/* Portable, versioned and checksummed control record; never store C padding. */
pxa_status_t pxa_app_locators_encode(uint64_t, const pxa_locator_publisher_t *, size_t,
                                     const pxa_locator_app_t *, size_t, uint8_t *, size_t,
                                     size_t *);
pxa_status_t pxa_app_locators_decode(pxa_app_locators_t *, const uint8_t *, size_t);
pxa_status_t pxa_app_locators_set_flags(pxa_app_locators_t *, const uint8_t root[32],
                                        const char *app_id, uint16_t flags);
/* Best-effort collection only after the signed executable has left the old
 * namespace and a volume-local record owns the installation. Failed writes
 * retain the old conservative mapping/cache, so unrelated legacy apps stay
 * usable even when NVS cannot accept a cleanup commit. No name is newly
 * assigned by this operation; publisher ownership bindings are retained. */
pxa_status_t pxa_app_locators_forget(pxa_app_locators_t *,const uint8_t root[32],const char *app_id);
#endif
