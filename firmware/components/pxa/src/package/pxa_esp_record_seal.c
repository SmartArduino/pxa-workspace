#include "pxa_esp_record_seal.h"
#include "pxa/wire.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include "nvs.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_key_lock=PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_device_key[32],g_key_ready;
static psa_key_id_t g_mac_key;
static const char key_namespace[]="pxa_security",key_name[]="seal_key_v1";

static int key_nonzero(void) {
    uint8_t any=0;
    for(unsigned i=0;i<sizeof(g_device_key);++i)any|=g_device_key[i];
    return any!=0;
}
static pxa_status_t activate_key(void) {
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status=psa_crypto_init();
    if(status==PSA_SUCCESS) {
        psa_set_key_type(&attributes,PSA_KEY_TYPE_HMAC);psa_set_key_bits(&attributes,256);
        psa_set_key_algorithm(&attributes,PSA_ALG_HMAC(PSA_ALG_SHA_256));
        psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_SIGN_MESSAGE);
        /* A single volatile provider key serves all app records. Its bytes
         * never persist in a PSA per-app table or remain in the read buffer. */
        status=psa_import_key(&attributes,g_device_key,sizeof(g_device_key),&g_mac_key);
    }
    psa_reset_key_attributes(&attributes);mbedtls_platform_zeroize(g_device_key,sizeof(g_device_key));
    g_key_ready=status==PSA_SUCCESS;
    return g_key_ready?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;
}
static pxa_status_t load_key(pxa_esp_record_entropy_fn entropy,void *context) {
    nvs_handle_t handle;size_t bytes=sizeof(g_device_key);esp_err_t error;
    if(g_key_ready)return PXA_STATUS_OK;
    error=nvs_open(key_namespace,NVS_READONLY,&handle);
    if(error==ESP_OK) {
        error=nvs_get_blob(handle,key_name,g_device_key,&bytes);nvs_close(handle);
        if(error==ESP_OK && bytes==sizeof(g_device_key) && key_nonzero())return activate_key();
        mbedtls_platform_zeroize(g_device_key,sizeof(g_device_key));
        if(error!=ESP_ERR_NVS_NOT_FOUND)return PXA_STATUS_DENIED;
    } else if(error!=ESP_ERR_NVS_NOT_FOUND)return PXA_STATUS_UNAVAILABLE;
    if(!entropy)return PXA_STATUS_UNAVAILABLE;
    error=nvs_open(key_namespace,NVS_READWRITE,&handle);
    if(error!=ESP_OK)return PXA_STATUS_UNAVAILABLE;
    pxa_status_t status=entropy(context,g_device_key);
    if(status!=PXA_STATUS_OK || !key_nonzero()) {
        mbedtls_platform_zeroize(g_device_key,sizeof(g_device_key));nvs_close(handle);
        return status==PXA_STATUS_OK?PXA_STATUS_UNAVAILABLE:status;
    }
    error=nvs_set_blob(handle,key_name,g_device_key,sizeof(g_device_key));
    if(error==ESP_OK)error=nvs_commit(handle);
    nvs_close(handle);
    if(error!=ESP_OK) {
        mbedtls_platform_zeroize(g_device_key,sizeof(g_device_key));return PXA_STATUS_UNAVAILABLE;
    }
    return activate_key();
}
pxa_status_t pxa_esp_record_seal_initialize(pxa_esp_record_entropy_fn entropy,void *context) {
    pxa_status_t status;
    pthread_mutex_lock(&g_key_lock);status=load_key(entropy,context);pthread_mutex_unlock(&g_key_lock);
    return status;
}
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,
    pxa_bytes_t record,uint8_t tag[PXA_ESP_RECORD_TAG_BYTES]) {
    static const uint8_t domain[]="PXA-HOST-RECORD-MAC";
    uint8_t lengths[12];pxa_status_t status;psa_mac_operation_t mac=PSA_MAC_OPERATION_INIT;
    size_t tag_bytes=0;
    if(!tag)return PXA_STATUS_INVALID_ARGUMENT;
    memset(tag,0,PXA_ESP_RECORD_TAG_BYTES);
    if(!purpose.data || !purpose.size || purpose.size>64 || !binding.data ||
        !binding.size || binding.size>512 || (!record.data && record.size) ||
        record.size>65536)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_write_u16(lengths,(uint16_t)purpose.size);pxa_write_u16(lengths+2,(uint16_t)binding.size);
    pxa_write_u64(lengths+4,(uint64_t)record.size);
    pthread_mutex_lock(&g_key_lock);status=load_key(NULL,NULL);
    if(status!=PXA_STATUS_OK) {pthread_mutex_unlock(&g_key_lock);return status;}
    if(psa_mac_sign_setup(&mac,g_mac_key,PSA_ALG_HMAC(PSA_ALG_SHA_256))!=PSA_SUCCESS ||
        psa_mac_update(&mac,domain,sizeof(domain))!=PSA_SUCCESS ||
        psa_mac_update(&mac,lengths,sizeof(lengths))!=PSA_SUCCESS ||
        psa_mac_update(&mac,purpose.data,purpose.size)!=PSA_SUCCESS ||
        psa_mac_update(&mac,binding.data,binding.size)!=PSA_SUCCESS ||
        (record.size && psa_mac_update(&mac,record.data,record.size)!=PSA_SUCCESS) ||
        psa_mac_sign_finish(&mac,tag,PXA_ESP_RECORD_TAG_BYTES,&tag_bytes)!=PSA_SUCCESS ||
        tag_bytes!=PXA_ESP_RECORD_TAG_BYTES)status=PXA_STATUS_INTERNAL;
    (void)psa_mac_abort(&mac);pthread_mutex_unlock(&g_key_lock);
    if(status!=PXA_STATUS_OK)memset(tag,0,PXA_ESP_RECORD_TAG_BYTES);
    return status;
}
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,
    pxa_bytes_t record,const uint8_t tag[PXA_ESP_RECORD_TAG_BYTES]) {
    uint8_t actual[PXA_ESP_RECORD_TAG_BYTES];volatile uint8_t different=0;
    pxa_status_t status;
    if(!tag)return PXA_STATUS_INVALID_ARGUMENT;
    status=pxa_esp_record_seal(purpose,binding,record,actual);
    if(status!=PXA_STATUS_OK)return status;
    for(unsigned i=0;i<sizeof(actual);++i)different|=actual[i]^tag[i];
    mbedtls_platform_zeroize(actual,sizeof(actual));
    return different?PXA_STATUS_DENIED:PXA_STATUS_OK;
}
