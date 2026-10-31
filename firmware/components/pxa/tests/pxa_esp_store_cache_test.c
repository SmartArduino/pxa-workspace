#define _GNU_SOURCE
#include "pxa_esp_store_cache.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_volumes.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/hmac.h>
pxa_status_t pxa_posix_fs_remove_tree(const char *path) {
    return !rmdir(path) || !unlink(path) || errno==ENOENT?PXA_STATUS_OK:PXA_STATUS_IO_ERROR;
}
static char mounts[2][256];
static unsigned references[2];
static int unavailable,key_unavailable;
void *heap_caps_malloc(size_t n,unsigned caps) {(void)caps;return malloc(n);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {(void)caps;return calloc(n,size);}
void heap_caps_free(void *p) {free(p);}
pxa_status_t pxa_esp_storage_resolve_volume(const char *path,pxa_volume_info_t *v) {
    for(unsigned i=0;i<2;++i)if(!strncmp(path,mounts[i],strlen(mounts[i])))return pxa_esp_storage_volume(i+1,v);
    return PXA_STATUS_NOT_FOUND;
}
bool pxa_esp_package_store_preview_file(const char *source,pxa_esp_package_preview_t *p) {
    (void)source;memset(p,0,sizeof(*p));strcpy(p->app_id,"fixture");p->has_publisher_root=true;p->publisher_root[0]=9;return true;
}
pxa_status_t pxa_esp_volumes_retain(uint64_t epoch) {
    assert(epoch>=1 && epoch<=2);if(unavailable)return PXA_STATUS_UNAVAILABLE;++references[epoch-1];return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_volumes_release(uint64_t epoch) {assert(references[epoch-1]);--references[epoch-1];return PXA_STATUS_OK;}
pxa_status_t pxa_esp_storage_volume_path(uint64_t epoch,char *out,size_t capacity) {
    assert(epoch>=1 && epoch<=2);if(unavailable)return PXA_STATUS_UNAVAILABLE;
    assert(snprintf(out,capacity,"%s",mounts[epoch-1])<(int)capacity);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *v) {
    assert(epoch>=1 && epoch<=2);memset(v,0,sizeof(*v));if(unavailable)return PXA_STATUS_UNAVAILABLE;
    v->id.bytes[0]=(uint8_t)epoch;v->mount_epoch=epoch;v->capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE;return PXA_STATUS_OK;
}
size_t pxa_esp_storage_volumes(pxa_volume_info_t *out,size_t capacity) {
    assert(capacity>=2);for(unsigned i=0;i<2;++i)assert(pxa_esp_storage_volume(i+1,&out[i])==PXA_STATUS_OK);return 2;
}
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t tag[32]) {
    if(key_unavailable)return PXA_STATUS_UNAVAILABLE;
    uint8_t input[512];size_t n=0;assert(purpose.size+binding.size+body.size<=sizeof(input));
    memcpy(input+n,purpose.data,purpose.size);n+=purpose.size;memcpy(input+n,binding.data,binding.size);n+=binding.size;
    memcpy(input+n,body.data,body.size);n+=body.size;unsigned result=0;assert(HMAC(EVP_sha256(),"fixture",7,input,n,tag,&result) && result==32);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,const uint8_t tag[32]) {
    uint8_t expected[32];pxa_status_t s=pxa_esp_record_seal(purpose,binding,body,expected);
    return s!=PXA_STATUS_OK?s:memcmp(expected,tag,32)?PXA_STATUS_DENIED:PXA_STATUS_OK;
}
static void copy(const char *from,const char *to) {
    uint8_t bytes[512];FILE *a=fopen(from,"rb"),*b=fopen(to,"wb");assert(a && b);size_t n;
    while((n=fread(bytes,1,sizeof(bytes),a)))assert(fwrite(bytes,1,n,b)==n);
    assert(!ferror(a) && !fclose(a) && !fclose(b));
}
static void pack(const pxa_esp_store_download_entry_t *entry,char path[384]) {
    assert(pxa_esp_store_cache_path(entry->epoch,entry->filename,path,384)==PXA_STATUS_OK);
    FILE *file=fopen(path,"wb");assert(file && fwrite("PXA",1,3,file)==3 && !fclose(file));
}
int main(void) {
    char root[]="/tmp/pxa-cache-XXXXXX",path[384],metadata[400],other[400];assert(mkdtemp(root));
    for(unsigned i=0;i<2;++i) {
        snprintf(mounts[i],sizeof(mounts[i]),"%s/volume%u",root,i);assert(!mkdir(mounts[i],0700));
        snprintf(path,sizeof(path),"%s/PXA",mounts[i]);assert(!mkdir(path,0700));
        snprintf(path,sizeof(path),"%s/PXA/cache",mounts[i]);assert(!mkdir(path,0700));
        snprintf(path,sizeof(path),"%s/PXA/cache/store",mounts[i]);assert(!mkdir(path,0700));
    }
    pxa_esp_store_download_entry_t entry={.filename=".store-1234abcd.pxa",.app_id="fixture",.owner="publisher:store",.epoch=2,.size=3};
    entry.digest[0]=17;entry.publisher[0]=9;pack(&entry,path);assert(pxa_esp_store_cache_save(&entry)==PXA_STATUS_OK);
    pxa_esp_store_download_entry_t found[8];pxa_esp_store_cache_load(found,8);
    assert(!strcmp(found[0].filename,entry.filename) && !strcmp(found[0].owner,entry.owner) && found[0].epoch==2 && found[0].size==3 && !memcmp(found[0].digest,entry.digest,32));
    snprintf(metadata,sizeof(metadata),"%s.meta",path);
    FILE *file=fopen(metadata,"r+b");assert(file && !fseek(file,93,SEEK_SET) && fputc('X',file)!=EOF && !fclose(file));
    pxa_esp_store_cache_load(found,8);assert(!found[0].filename[0] && !access(path,F_OK)); /* Tamper quarantined, preserved. */
    assert(pxa_esp_store_cache_save(&entry)==PXA_STATUS_OK);
    key_unavailable=1;pxa_esp_store_cache_load(found,8);assert(!found[0].filename[0] && !access(path,F_OK));key_unavailable=0;
    pxa_esp_store_download_entry_t foreign=entry;foreign.epoch=1;pack(&foreign,other);strcat(other,".meta");copy(metadata,other);
    pxa_esp_store_cache_load(found,8);assert(found[0].epoch==2 && !found[1].filename[0]); /* Volume-bound MAC. */
    assert(pxa_esp_store_cache_save(&foreign)==PXA_STATUS_OK);pxa_esp_store_cache_load(found,8);
    assert(found[0].filename[0] && !found[0].owner[0] && !found[1].filename[0]); /* Duplicate token is ambiguous. */
    assert(pxa_esp_store_cache_remove(&foreign)==PXA_STATUS_OK);
    pxa_esp_store_download_entry_t orphan=entry;strcpy(orphan.filename,".store-87654321.pxa");pack(&orphan,path);
    pxa_esp_store_cache_load(found,8);assert(!access(path,F_OK)); /* Discovery cannot delete a publishing transfer. */
    char partial[400];snprintf(partial,sizeof(partial),"%s/PXA/cache/store/.stream-12345678",mounts[1]);
    assert(!mkdir(partial,0700));pxa_esp_store_cache_load(found,8);assert(!access(partial,F_OK));
    pxa_esp_store_cache_cleanup();assert(access(partial,F_OK)!=0);pxa_esp_store_cache_load(found,8);assert(!access(metadata,F_OK) && access(path,F_OK) && found[0].epoch==2);
    char token[20];pack(&orphan,path);assert(pxa_esp_store_cache_adopt(path,entry.owner,token,sizeof(token))==PXA_STATUS_OK);
    assert(!strcmp(token,orphan.filename));pxa_esp_store_cache_load(found,8);assert(found[1].filename[0]);
    assert(pxa_esp_store_cache_remove(&orphan)==PXA_STATUS_OK);
    unavailable=1;assert(pxa_esp_store_cache_remove(&entry)==PXA_STATUS_UNAVAILABLE);unavailable=0;
    assert(pxa_esp_store_cache_remove(&entry)==PXA_STATUS_OK && !references[0] && !references[1]);
    for(unsigned i=0;i<2;++i) {
        snprintf(path,sizeof(path),"%s/PXA/cache/store",mounts[i]);assert(!rmdir(path));
        snprintf(path,sizeof(path),"%s/PXA/cache",mounts[i]);assert(!rmdir(path));
        snprintf(path,sizeof(path),"%s/PXA",mounts[i]);assert(!rmdir(path));assert(!rmdir(mounts[i]));
    }
    assert(!rmdir(root));puts("Store cache: volume-local discovery, MAC tamper/replay refusal, offline key, duplicate tokens, orphan cleanup and no primary registry passed");
}
