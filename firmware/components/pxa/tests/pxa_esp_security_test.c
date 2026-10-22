#include "../src/package/pxa_esp_security.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pxa_status_t initial,provision;
static unsigned calls,enables,disables,randoms;
static int enabled;
void bootloader_random_enable(void) {assert(!enabled);enabled=1;++enables;}
void bootloader_random_disable(void) {assert(enabled);enabled=0;++disables;}
void esp_fill_random(void *output,size_t bytes) {
    assert(enabled && bytes==32);memset(output,0x42,bytes);++randoms;
}
pxa_status_t pxa_esp_record_seal_initialize(pxa_esp_record_entropy_fn entropy,void *context) {
    ++calls;
    if(!entropy) {assert(calls==1 && !enabled);return initial;}
    assert(calls==2 && enabled);
    if(provision==PXA_STATUS_OK) {
        uint8_t key[32]={0};assert(entropy(context,key)==PXA_STATUS_OK);
        for(unsigned i=0;i<sizeof(key);++i)assert(key[i]==0x42);
    }
    return provision;
}
static void reset(pxa_status_t first,pxa_status_t next) {
    assert(!enabled);initial=first;provision=next;calls=enables=disables=randoms=0;
}
int main(void) {
    reset(PXA_STATUS_OK,PXA_STATUS_INTERNAL);
    assert(!pxa_esp_security_initialize_early() && calls==1 && !enables && !randoms);
    reset(PXA_STATUS_DENIED,PXA_STATUS_OK);
    assert(pxa_esp_security_initialize_early()==PXA_STATUS_DENIED && calls==1 && !enables);
    reset(PXA_STATUS_UNAVAILABLE,PXA_STATUS_OK);
    assert(!pxa_esp_security_initialize_early() && calls==2 && enables==1 && disables==1 && randoms==1 && !enabled);
    reset(PXA_STATUS_UNAVAILABLE,PXA_STATUS_UNAVAILABLE);
    assert(pxa_esp_security_initialize_early()==PXA_STATUS_UNAVAILABLE && calls==2 && enables==1 && disables==1 && !enabled);
    puts("Early security: existing/corrupt key preservation and bounded entropy-source lifetime on success/failure passed");
    return 0;
}
