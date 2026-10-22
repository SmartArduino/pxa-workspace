/* Include the implementation to model a cold process/device without exposing
 * a production API which could reset or replace the cached device key. */
#include "../src/package/pxa_esp_record_seal.c"
#include <assert.h>
#include <stdio.h>

static uint8_t stored[32],staged[32];
static size_t stored_size;
static unsigned entropy_calls,write_opens,writes,commits;
static int namespace_present,staged_present,fail_write,fail_commit,fail_open,fail_entropy;

esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle) {
    assert(!strcmp(name,"pxa_security"));
    if(mode==NVS_READONLY) {
        if(!namespace_present)return ESP_ERR_NVS_NOT_FOUND;
        *handle=1;return ESP_OK;
    }
    assert(mode==NVS_READWRITE);++write_opens;
    if(fail_open)return ESP_FAIL;
    namespace_present=1;*handle=2;return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *data,size_t *size) {
    assert(handle==1 && !strcmp(key,"seal_key_v1"));
    if(!stored_size)return ESP_ERR_NVS_NOT_FOUND;
    if(*size<stored_size) {*size=stored_size;return ESP_ERR_NVS_INVALID_LENGTH;}
    memcpy(data,stored,stored_size);*size=stored_size;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *data,size_t size) {
    assert(handle==2 && !strcmp(key,"seal_key_v1") && size==32);++writes;
    if(fail_write)return ESP_FAIL;
    memcpy(staged,data,size);staged_present=1;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle==2 && staged_present);++commits;
    if(fail_commit)return ESP_FAIL;
    memcpy(stored,staged,32);stored_size=32;return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {assert(handle==1 || handle==2);if(handle==2)staged_present=0;}
static pxa_status_t entropy(void *context,uint8_t key[32]) {
    assert(context==&entropy_calls);++entropy_calls;
    if(fail_entropy)return PXA_STATUS_UNAVAILABLE;
    for(unsigned i=0;i<32;++i)key[i]=(uint8_t)(i+1);
    return PXA_STATUS_OK;
}
static pxa_bytes_t bytes(const char *s) {return (pxa_bytes_t){(const uint8_t *)s,strlen(s)};}
static void cold_start(void) {
    if(g_key_ready)assert(psa_destroy_key(g_mac_key)==PSA_SUCCESS);
    g_mac_key=0;g_key_ready=0;mbedtls_platform_zeroize(g_device_key,sizeof(g_device_key));
}
int main(void) {
    pxa_bytes_t purpose=bytes("install/1"),binding=bytes("volume:test/install:test"),record=bytes("trusted-test-record");
    uint8_t tag[32],bad[32];char hexadecimal[65];unsigned old_writes,old_commits,old_entropy,old_opens;
    assert(pxa_esp_record_seal(purpose,binding,record,tag)==PXA_STATUS_UNAVAILABLE && !write_opens);
    for(unsigned i=0;i<32;++i)assert(!tag[i]);
    fail_entropy=1;
    assert(pxa_esp_record_seal_initialize(entropy,&entropy_calls)==PXA_STATUS_UNAVAILABLE && !stored_size);
    fail_entropy=0;fail_write=1;
    assert(pxa_esp_record_seal_initialize(entropy,&entropy_calls)==PXA_STATUS_UNAVAILABLE && !stored_size);
    fail_write=0;fail_commit=1;
    assert(pxa_esp_record_seal_initialize(entropy,&entropy_calls)==PXA_STATUS_UNAVAILABLE && !stored_size);
    fail_commit=0;
    assert(!pxa_esp_record_seal_initialize(entropy,&entropy_calls) && stored_size==32);
    assert(!pxa_esp_record_seal(purpose,binding,record,tag));
    for(unsigned i=0;i<32;++i)snprintf(hexadecimal+2*i,3,"%02x",tag[i]);
    /* Independent Python hashlib/hmac vector for this public test key/frame. */
    assert(!strcmp(hexadecimal,"3db8b685efd74f62b2234c1c9c045572cf3d7ece77eab0c724d35567cd723078"));
    assert(!pxa_esp_record_verify(purpose,binding,record,tag));
    for(unsigned i=0;i<32;++i) {
        memcpy(bad,tag,32);bad[i]^=1;
        assert(pxa_esp_record_verify(purpose,binding,record,bad)==PXA_STATUS_DENIED);
    }
    assert(pxa_esp_record_verify(bytes("grants/1"),binding,record,tag)==PXA_STATUS_DENIED);
    assert(pxa_esp_record_verify(purpose,bytes("volume:other/install:test"),record,tag)==PXA_STATUS_DENIED);
    assert(pxa_esp_record_verify(purpose,binding,bytes("modified-test-record"),tag)==PXA_STATUS_DENIED);
    assert(!pxa_esp_record_seal(bytes("a"),bytes("bc"),record,bad));
    assert(pxa_esp_record_verify(bytes("ab"),bytes("c"),record,bad)==PXA_STATUS_DENIED);
    old_writes=writes;old_commits=commits;old_entropy=entropy_calls;old_opens=write_opens;
    for(unsigned i=0;i<100;++i)assert(!pxa_esp_record_seal(purpose,binding,record,bad));
    assert(writes==old_writes && commits==old_commits && entropy_calls==old_entropy && write_opens==old_opens);
    /* Existing keys load using read-only NVS even when no more NVS entries
     * can be created. Installing more applications never grows that table. */
    fail_open=1;cold_start();
    assert(!pxa_esp_record_verify(purpose,binding,record,tag) && write_opens==old_opens);
    assert(!pxa_esp_record_seal_initialize(NULL,NULL));
    for(unsigned i=0;i<32;++i)stored[i]^=128;
    cold_start();assert(pxa_esp_record_verify(purpose,binding,record,tag)==PXA_STATUS_DENIED);
    stored_size=31;cold_start();
    assert(pxa_esp_record_seal_initialize(entropy,&entropy_calls)==PXA_STATUS_DENIED && entropy_calls==old_entropy);
    memset(stored,0,32);stored_size=32;cold_start();
    assert(pxa_esp_record_seal_initialize(entropy,&entropy_calls)==PXA_STATUS_DENIED && entropy_calls==old_entropy);
    assert(writes==old_writes && commits==old_commits);
    assert(pxa_esp_record_seal((pxa_bytes_t){NULL,0},binding,record,bad)==PXA_STATUS_INVALID_ARGUMENT);
    assert(pxa_esp_record_seal(purpose,binding,(pxa_bytes_t){record.data,65537},bad)==PXA_STATUS_INVALID_ARGUMENT);
    cold_start();
    puts("Record MAC: independent vector, bindings, corruption, device copy, provisioning failures and constant NVS footprint passed");
    return 0;
}
