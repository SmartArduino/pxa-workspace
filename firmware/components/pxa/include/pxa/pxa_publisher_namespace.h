#ifndef PXA_PUBLISHER_NAMESPACE_POLICY_H
#define PXA_PUBLISHER_NAMESPACE_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Product-owned policy, separate from application package signing trust.
 * authority_id stays constant when that issuer rotates its signing key.
 * Applications and network responses cannot add entries to this table. */
typedef struct {
    const uint8_t *spki;
    size_t spki_bytes;
    uint8_t authority_id[32];
} PxaPublisherNamespaceIssuer;
/* Optional product hook. An absent hook grants no namespace authority. */
size_t pxa_platform_trusted_namespace_issuers(const PxaPublisherNamespaceIssuer **);
#ifdef __cplusplus
}
#endif
#endif
