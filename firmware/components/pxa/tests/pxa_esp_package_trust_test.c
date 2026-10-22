#include "../src/package/pxa_esp_package_trust.c"
#include <assert.h>
#include <stdio.h>

static const uint8_t product_der[]={1},development_der[]={2};
static PxaTrustedPublisherKey product_keys[]={{product_der,1},{product_der,1}};
static const PxaTrustedPublisherKey development_keys[]={{development_der,1}};
static unsigned live_allocations,allocations,product_reads,development_reads;
static int null_product,fail_allocation;
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    void *p;(void)caps;++allocations;
    if(fail_allocation)return NULL;
    p=calloc(n,bytes);if(p)++live_allocations;return p;
}
void heap_caps_free(void *p) {if(p) {assert(live_allocations);--live_allocations;}free(p);}
size_t pxa_platform_trusted_publishers(const PxaTrustedPublisherKey **output) {
    ++product_reads;*output=null_product?NULL:product_keys;return 2;
}
size_t pxa_bundled_development_publishers(const PxaTrustedPublisherKey **output) {
    ++development_reads;*output=development_keys;return 1;
}
pxa_status_t pxa_esp_mbedtls_p256_verify(void *context,pxa_bytes_t key,pxa_bytes_t domain,
    pxa_bytes_t manifest,pxa_bytes_t signature) {
    (void)context;(void)key;(void)domain;(void)manifest;(void)signature;
    assert(!"profile fixture must not replace the real signature tests");return PXA_STATUS_DENIED;
}
int main(void) {
    pxa_posix_installer_config_t first,second;
    pxa_esp_mbedtls_trust_t first_context,second_context;
    unsigned created,reads;
    assert(pxa_esp_package_installer_profile(NULL,&first_context)==PXA_STATUS_INVALID_ARGUMENT);
    null_product=1;
    assert(pxa_esp_package_installer_profile(&first,&first_context)==PXA_STATUS_INVALID_ARGUMENT && !live_allocations);
    null_product=0;fail_allocation=1;
    assert(pxa_esp_package_installer_profile(&first,&first_context)==PXA_STATUS_RESOURCE_LIMIT && !live_allocations);
    fail_allocation=0;product_keys[1].spki_der_size=0;
    assert(pxa_esp_package_installer_profile(&first,&first_context)==PXA_STATUS_INVALID_ARGUMENT && !live_allocations);
    product_keys[1].spki_der_size=1;
    assert(!pxa_esp_package_installer_profile(&first,&first_context) && live_allocations==1);
    created=allocations;reads=product_reads;
    assert(first.trust.key_count==3 && first_context.key_count==3);
    assert(first_context.keys[0].spki==product_der && first_context.keys[2].spki==development_der);
    assert(first.verify==pxa_esp_mbedtls_p256_verify && first.verify_context==&first_context);
    /* Model an embedded publisher key cached between manifest and container
     * verification. A concurrent operation must never borrow that pointer. */
    first_context.open_distribution_spki=development_der;
    first_context.open_distribution_spki_size=1;first_context.has_open_distribution_key=1;
    assert(!pxa_esp_package_installer_profile(&second,&second_context));
    assert(second.verify_context==&second_context && first.verify_context!=second.verify_context);
    assert(first_context.keys==second_context.keys && !second_context.open_distribution_spki &&
        !second_context.has_open_distribution_key && first_context.has_open_distribution_key);
    assert(allocations==created && product_reads==reads && development_reads==reads);
    assert(!first.storage_root && !second.storage_root); /* No default filesystem required. */
    heap_caps_free((void *)g_keys);g_keys=NULL;g_key_count=0;g_ready=0;
    assert(!live_allocations);
    puts("Installer trust: no filesystem dependency, shared immutable anchors and isolated transient verifier state passed");
    return 0;
}
