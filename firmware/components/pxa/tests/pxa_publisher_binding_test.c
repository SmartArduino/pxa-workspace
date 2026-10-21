#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pxa_publisher_binding.h"
#include "pxa/openssl/pxa_openssl.h"
#include "pxa/wire.h"
typedef struct { pxa_openssl_trust_t trust;uint8_t key_id[32];int denied; } policy_t;
static pxa_status_t authority(void *context,const uint8_t key[32],uint8_t out[32]) {
    policy_t *p=context;if(p->denied || memcmp(key,p->key_id,32))return PXA_STATUS_DENIED;
    memcpy(out,p->key_id,32);return PXA_STATUS_OK;
}
static pxa_status_t verify(void *context,pxa_bytes_t key,pxa_bytes_t domain,pxa_bytes_t message,pxa_bytes_t signature) {
    return pxa_openssl_p256_verify(&((policy_t *)context)->trust,key,domain,message,signature);
}
static size_t load(const char *path,uint8_t *out,size_t capacity) {
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,capacity,f);assert(!ferror(f) && feof(f));fclose(f);return n;
}
int main(int argc,char **argv) {
    assert(argc==3 || argc==4);uint8_t token[397],spki[128];
    size_t size=load(argv[1],token,sizeof(token)),spki_size=load(argv[2],spki,sizeof(spki));
    pxa_openssl_publisher_key_t key={spki,spki_size};policy_t policy={0};
    policy.trust=(pxa_openssl_trust_t){sizeof(policy.trust),&key,1};
    assert(pxa_openssl_sha256(spki,spki_size,policy.key_id)==PXA_STATUS_OK);
    pxa_publisher_binding_t binding;uint64_t now=pxa_read_u64(token+96)+1;
    if(argc==4){assert(pxa_publisher_binding_verify(token,size,now,&policy,authority,verify,&binding)!=PXA_STATUS_OK);return 0;}
    assert(pxa_publisher_binding_verify(token,size,now,&policy,authority,verify,&binding)==PXA_STATUS_OK);
    assert(!strcmp(binding.app_id,"pxa-nes") && !strcmp(binding.profile.handle,"lucinhu"));
    assert(binding.revision==UINT64_C(9007199254740993));
    uint64_t expires=binding.expires_at;
    assert(pxa_publisher_binding_verify(token,size,0,&policy,authority,verify,&binding)==PXA_STATUS_UNAVAILABLE);
    assert(pxa_publisher_binding_verify(token,size,expires,&policy,authority,verify,&binding)==PXA_STATUS_DENIED);
    policy.denied=1;assert(pxa_publisher_binding_verify(token,size,now,&policy,authority,verify,&binding)==PXA_STATUS_DENIED);policy.denied=0;
    for(size_t i=0;i<size;++i){token[i]^=1;assert(pxa_publisher_binding_verify(token,size,now,&policy,authority,verify,&binding)!=PXA_STATUS_OK);token[i]^=1;}
    for(size_t i=0;i<size;++i)assert(pxa_publisher_binding_verify(token,i,now,&policy,authority,verify,&binding)!=PXA_STATUS_OK);
    puts("PASS: real server P-256 binding, exact owner, >JS-safe revision, unknown issuer, expiry, every-byte mutation and truncation");
}
