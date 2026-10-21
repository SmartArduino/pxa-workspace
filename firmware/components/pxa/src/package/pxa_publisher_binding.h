#ifndef PXA_PUBLISHER_BINDING_H
#define PXA_PUBLISHER_BINDING_H
#include "pxa/package.h"
#include "pxa_publisher_profile.h"
#define PXA_PUBLISHER_BINDING_MAX 396u
#define PXA_PUBLISHER_BINDING_DOMAIN "PXA-PUBLISHER-BINDING-BINARY-V1"
#define PXA_PUBLISHER_NAMESPACE_DOMAIN "PXA-PUBLISHER-NAMESPACE-1"
typedef struct {
    uint8_t root[32], authority[32], namespace_id[16];
    char app_id[65];
    pxa_publisher_profile_t profile;
    uint64_t revision, issued_at, expires_at;
} pxa_publisher_binding_t;
/* Product policy maps an allowed signing key to a stable issuer authority.
 * Never use the token's key ID itself as an implicit trust-store entry. */
typedef pxa_status_t (*pxa_publisher_authority_fn)(void *, const uint8_t[32], uint8_t[32]);
pxa_status_t pxa_publisher_binding_verify(const uint8_t *, size_t, uint64_t now_utc, void *,
                                          pxa_publisher_authority_fn,
                                          pxa_package_signature_verify_fn,
                                          pxa_publisher_binding_t *);
#endif
