#include "pxa_esp_package_trust.h"
#include "pxa/esp/pxa_esp_mbedtls.h"
#include "pxa/pxa_publisher_trust.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include <pthread.h>
#include <string.h>

size_t pxa_platform_trusted_publishers(const PxaTrustedPublisherKey **) __attribute__((weak));
#if CONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY
size_t pxa_bundled_development_publishers(const PxaTrustedPublisherKey **);
#endif
static pthread_mutex_t g_trust_lock=PTHREAD_MUTEX_INITIALIZER;
static const pxa_esp_mbedtls_publisher_key_t *g_keys;
static size_t g_key_count;
static uint8_t g_ready;

static pxa_status_t initialize_trust(void) {
    const PxaTrustedPublisherKey *product=NULL,*development=NULL;
    size_t product_count=0,development_count=0,total;
    pxa_esp_mbedtls_publisher_key_t *keys=NULL;
    if(g_ready)return PXA_STATUS_OK;
    if(pxa_platform_trusted_publishers)product_count=pxa_platform_trusted_publishers(&product);
#if CONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY
    development_count=pxa_bundled_development_publishers(&development);
#endif
    if((product_count && !product) || (development_count && !development))return PXA_STATUS_INVALID_ARGUMENT;
    if(product_count>SIZE_MAX-development_count)return PXA_STATUS_RESOURCE_LIMIT;
    total=product_count+development_count;
    if(total>SIZE_MAX/sizeof(*keys))return PXA_STATUS_RESOURCE_LIMIT;
    if(total) {
        keys=heap_caps_calloc(total,sizeof(*keys),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!keys)keys=heap_caps_calloc(total,sizeof(*keys),MALLOC_CAP_8BIT);
        if(!keys)return PXA_STATUS_RESOURCE_LIMIT;
    }
    for(size_t i=0;i<total;++i) {
        const PxaTrustedPublisherKey *source=i<product_count?&product[i]:&development[i-product_count];
        if(!source->spki_der || !source->spki_der_size) {heap_caps_free(keys);return PXA_STATUS_INVALID_ARGUMENT;}
        keys[i]=(pxa_esp_mbedtls_publisher_key_t){source->spki_der,source->spki_der_size};
    }
    g_keys=keys;g_key_count=total;g_ready=1;
    return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_installer_profile(pxa_posix_installer_config_t *profile,
    pxa_esp_mbedtls_trust_t *verification_context) {
    pxa_status_t status;
    if(!profile || !verification_context)return PXA_STATUS_INVALID_ARGUMENT;
    memset(profile,0,sizeof(*profile));pthread_mutex_lock(&g_trust_lock);
    status=initialize_trust();
    if(status==PXA_STATUS_OK) {
        profile->struct_size=sizeof(*profile);
        memset(verification_context,0,sizeof(*verification_context));
        verification_context->struct_size=sizeof(*verification_context);
        verification_context->keys=g_keys;verification_context->key_count=g_key_count;
        profile->trust=(pxa_openssl_trust_t){sizeof(profile->trust),
            (const pxa_openssl_publisher_key_t *)g_keys,g_key_count};
        profile->flags=PXA_POSIX_INSTALLER_FLAG_SKIP_LOCK|PXA_POSIX_INSTALLER_FLAG_REPAIR_CORRUPT|
            PXA_POSIX_INSTALLER_FLAG_COMPOSITE_IDENTITY;
        profile->verify=pxa_esp_mbedtls_p256_verify;profile->verify_context=verification_context;
        pxa_package_limits_init(&profile->limits);
    }
    pthread_mutex_unlock(&g_trust_lock);return status;
}
