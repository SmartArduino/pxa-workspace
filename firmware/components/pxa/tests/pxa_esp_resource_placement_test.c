// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "pxa/install_record.h"
#include "pxa/package.h"
#include "pxa/pxa_esp_storage.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static pxa_volume_info_t volumes[2];
static unsigned exists_mask, denied_mask, visited_mask;
static int fail_allocation;
static void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    (void)caps;return fail_allocation?NULL:calloc(n,bytes);
}
static void heap_caps_free(void *p) {free(p);}
size_t pxa_esp_storage_volumes(pxa_volume_info_t *out,size_t count) {
    assert(count>=2);memcpy(out,volumes,sizeof(volumes));return 2;
}
pxa_status_t pxa_esp_storage_volume_path(uint64_t epoch,char *out,size_t capacity) {
    int n=snprintf(out,capacity,"/volume-%llu",(unsigned long long)epoch);
    return n<0 || (size_t)n>=capacity?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
static pxa_status_t pxa_esp_installations_media_path(const pxa_install_record_t *record,char *out,size_t capacity) {
    unsigned id=record->media_volume.bytes[0];
    if(id<1 || id>2)return PXA_STATUS_INVALID_ARGUMENT;
    visited_mask|=1u<<id;
    if(denied_mask&(1u<<id))return PXA_STATUS_DENIED;
    // The same signed publisher has different native directory names per volume.
    snprintf(out,capacity,"PXA/media/publisher-%u/app/generation/package",id);
    return PXA_STATUS_OK;
}
static int test_stat(const char *path,struct stat *out) {
    unsigned id=0,alias=0;
    if(sscanf(path,"/volume-%u/PXA/media/publisher-%u/app/generation/package",&id,&alias)!=2 ||
       id!=alias || !(exists_mask&(1u<<id)))return -1;
    memset(out,0,sizeof(*out));out->st_mode=S_IFDIR;return 0;
}
#define stat(path,out) test_stat(path,out)
// PRODUCTION
#undef stat

int main(void) {
    uint8_t digest[32]={1};
    pxa_package_manifest_t manifest={0};manifest.format_minor=8;
    manifest.resource_manifest_sha256=digest;
    for(unsigned i=0;i<2;++i) {
        volumes[i].id.bytes[0]=i+1;volumes[i].mount_epoch=i+1;
        volumes[i].state=PXA_VOLUME_READY;volumes[i].capabilities=PXA_VOLUME_READ;
    }
    pxa_install_record_t record={0};record.install_id[0]=17;record.data_generation=9;
    exists_mask=1u<<2;
    assert(bind_prepared_media(&manifest,2,&record)==PXA_STATUS_OK);
    assert(record.media_volume.bytes[0]==2 && record.data_generation==9 && record.install_id[0]==17);
    assert(!memcmp(record.media_set_sha256,digest,32));
    // An unavailable previous volume must not reuse its alias on the next one.
    record.media_volume=volumes[0].id;visited_mask=0;
    assert(bind_prepared_media(&manifest,1,&record)==PXA_STATUS_OK);
    assert(record.media_volume.bytes[0]==2 && visited_mask==((1u<<1)|(1u<<2)));
    // Preserve the current resource volume ahead of the application volume.
    record.media_volume=volumes[0].id;exists_mask=(1u<<1)|(1u<<2);
    assert(bind_prepared_media(&manifest,2,&record)==PXA_STATUS_OK && record.media_volume.bytes[0]==1);
    denied_mask=1u<<1;
    assert(bind_prepared_media(&manifest,1,&record)==PXA_STATUS_OK && record.media_volume.bytes[0]==2);
    exists_mask=0;record.media_volume=volumes[0].id;
    assert(bind_prepared_media(&manifest,2,&record)==PXA_STATUS_UNAVAILABLE && record.media_volume.bytes[0]==1);
    fail_allocation=1;
    assert(bind_prepared_media(&manifest,2,&record)==PXA_STATUS_RESOURCE_LIMIT);
    puts("PASS: new resource volume, per-volume aliases, previous placement, missing media and allocation failure");
}
