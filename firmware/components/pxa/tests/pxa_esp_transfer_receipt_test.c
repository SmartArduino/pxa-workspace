#define _GNU_SOURCE
#include "pxa_esp_transfer_receipt.h"
#include "pxa_esp_record_seal.h"
#include "pxa/pxa_esp_storage.h"
#include "record_test_auth.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned sync_calls,fail_sync_at;
static int no_replace,fail_publish;
static int injected_sync(int file);
static int injected_rename(int,const char *,int,const char *);
#define fsync injected_sync
#define renameat injected_rename
#include "../src/package/pxa_esp_transfer_receipt.c"
#undef fsync
#undef renameat
static int injected_rename(int from,const char *old,int to,const char *name) {
    struct stat metadata;
    if(no_replace && fstatat(to,name,&metadata,AT_SYMLINK_NOFOLLOW)==0) {errno=EEXIST;return -1;}
    if(fail_publish) {fail_publish=0;errno=EIO;return -1;}
    return renameat(from,old,to,name);
}
static int injected_sync(int file) {
    ++sync_calls;if(sync_calls==fail_sync_at) {errno=EIO;return -1;}return fsync(file);
}
static pxa_volume_registry_t *registry;
static record_test_auth_context_t key={{1},1};
static int read_only;
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *info) {
    pxa_status_t status=pxa_volume_get(registry,epoch,info);
    if(status==PXA_STATUS_OK && read_only)info->capabilities&=~PXA_VOLUME_WRITE;
    return status;
}
pxa_status_t pxa_esp_record_seal(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t record,uint8_t tag[32]) {
    return record_test_tag(&key,purpose,binding,record,tag);
}
pxa_status_t pxa_esp_record_verify(pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t record,const uint8_t tag[32]) {
    uint8_t expected[32];pxa_status_t status=record_test_tag(&key,purpose,binding,record,expected);
    return status==PXA_STATUS_OK && memcmp(expected,tag,32)?PXA_STATUS_DENIED:status;
}
static pxa_esp_transfer_receipt_t fixture(void) {
    pxa_esp_transfer_receipt_t r={0};r.transfer.revision=1;r.transfer.phase=PXA_INSTALL_TRANSFER_COPYING;
    r.transfer.transfer_id[0]=9;r.transfer.target_app_volume.bytes[0]=7;r.transfer.target_media_volume.bytes[0]=2;
    r.transfer.source.generation=8;r.transfer.source.data_generation=1;r.transfer.source.release_sequence=201;
    r.transfer.source.flags=PXA_INSTALL_RECORD_ENABLED|PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
    r.transfer.source.app_volume.bytes[0]=1;r.transfer.source.media_volume.bytes[0]=2;
    r.transfer.source.publisher_root[0]=1;r.transfer.source.install_id[0]=3;
    r.transfer.source.manifest_sha256[0]=4;r.transfer.source.media_set_sha256[0]=5;
    strcpy(r.transfer.source.app_id,"quake");r.context_size=8;memcpy(r.context,"legacy01",8);return r;
}
static size_t raw(int directory,unsigned slot,uint8_t bytes[RECEIPT_MAX]) {
    int file=openat(directory,slots[slot],O_RDONLY);assert(file>=0);
    ssize_t n=read(file,bytes,RECEIPT_MAX);assert(n>0 && !close(file));return (size_t)n;
}
static void mirrored(int directory) {
    uint8_t a[RECEIPT_MAX],b[RECEIPT_MAX];size_t n=raw(directory,0,a);assert(n==raw(directory,1,b) && !memcmp(a,b,n));
}
int main(void) {
    size_t bytes=pxa_volume_registry_workspace_size(2);void *volume_memory=calloc(1,bytes);assert(volume_memory);
    assert(!pxa_volume_registry_init(volume_memory,bytes,2,100,&registry));
    pxa_volume_info_t volume={0};uint64_t source_epoch,target_epoch;
    volume.id.bytes[0]=1;volume.device_id.bytes[0]=1;volume.state=PXA_VOLUME_READY;
    volume.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE;assert(!pxa_volume_register(registry,&volume,&source_epoch));
    volume.id.bytes[0]=7;volume.device_id.bytes[0]=7;assert(!pxa_volume_register(registry,&volume,&target_epoch));
    char path[256];assert(snprintf(path,sizeof(path),"%s/transfer-receipt-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp")<(int)sizeof(path));
    assert(mkdtemp(path));int directory=open(path,O_RDONLY|O_DIRECTORY);assert(directory>=0);
    char locator[130];memset(locator,'0',64);locator[1]='1';strcpy(locator+64,"/quake");
    pxa_esp_transfer_receipt_config_t config={directory,source_epoch,PXA_ESP_TRANSFER_SOURCE,locator};
    size_t size=pxa_esp_transfer_receipt_workspace_size();void *memory=calloc(1,size);assert(memory);
    pxa_esp_transfer_receipt_t receipt=fixture(),loaded,next;uint8_t durable;
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,&loaded)==PXA_STATUS_NOT_FOUND && !loaded.transfer.revision);
    assert(pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size-1,&durable)==PXA_STATUS_RESOURCE_LIMIT && !durable);
    key.available=0;
    assert(pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size,&durable)==PXA_STATUS_UNAVAILABLE && !durable);
    assert(faccessat(directory,PXA_ESP_TRANSFER_SLOT_0,F_OK,0)<0 && faccessat(directory,PXA_ESP_TRANSFER_SLOT_1,F_OK,0)<0);
    key.available=1;
    assert(!pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size,&durable) && durable);mirrored(directory);
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded));
    assert(loaded.transfer.revision==1 && loaded.context_size==8 && !memcmp(loaded.context,"legacy01",8));
    assert(pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size,&durable)==PXA_STATUS_BAD_STATE && !durable);
    pxa_esp_transfer_receipt_config_t foreign=config;foreign.mount_epoch=target_epoch;
    assert(pxa_esp_transfer_receipt_load(&foreign,memory,size,&loaded)==PXA_STATUS_DENIED && !loaded.transfer.revision);
    foreign=config;foreign.role=PXA_ESP_TRANSFER_TARGET;
    assert(pxa_esp_transfer_receipt_load(&foreign,memory,size,&loaded)==PXA_STATUS_DENIED);
    char other[130];strcpy(other,locator);strcpy(other+65,"other");foreign=config;foreign.locator=other;
    assert(pxa_esp_transfer_receipt_load(&foreign,memory,size,&loaded)==PXA_STATUS_DENIED);
    key.key[0]=2;
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,&loaded)==PXA_STATUS_DENIED);key.key[0]=1;
    uint8_t encoded[RECEIPT_MAX];size_t encoded_size=raw(directory,0,encoded);encoded[RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES]^=1;
    int file=openat(directory,slots[0],O_WRONLY);assert(file>=0 && write(file,encoded,encoded_size)==(ssize_t)encoded_size && !close(file));
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded) && loaded.transfer.revision==1);
    assert(!pxa_esp_transfer_receipt_store(&config,1,&receipt,memory,size,&durable) && durable);mirrored(directory);
    next=receipt;assert(!pxa_install_transfer_advance(&receipt.transfer,PXA_INSTALL_TRANSFER_READY,&next.transfer));
    fail_sync_at=sync_calls+3; /* New slot durable; failure while mirroring. */
    assert(pxa_esp_transfer_receipt_store(&config,1,&next,memory,size,&durable)==PXA_STATUS_IO_ERROR && durable);
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded) && loaded.transfer.revision==2);
    fail_sync_at=0;
    assert(pxa_esp_transfer_receipt_store(&config,1,&next,memory,size,&durable)==PXA_STATUS_BAD_STATE && !durable);
    assert(!pxa_esp_transfer_receipt_store(&config,2,&next,memory,size,&durable) && durable);mirrored(directory);
    receipt=next;assert(!pxa_install_transfer_advance(&receipt.transfer,PXA_INSTALL_TRANSFER_SURRENDERED,&next.transfer));
    fail_sync_at=sync_calls+2; /* Rename happened, directory sync uncertain. */
    assert(pxa_esp_transfer_receipt_store(&config,2,&next,memory,size,&durable)==PXA_STATUS_IO_ERROR && !durable);
    fail_sync_at=0;
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded) && loaded.transfer.revision==3);
    assert(!pxa_esp_transfer_receipt_store(&config,3,&next,memory,size,&durable) && durable);mirrored(directory);
    pxa_esp_transfer_receipt_t changed=next;changed.context[0]^=1;unsigned before=sync_calls;
    assert(pxa_esp_transfer_receipt_store(&config,3,&changed,memory,size,&durable)==PXA_STATUS_BAD_STATE && sync_calls==before);
    changed=next;changed.transfer.source.data_generation++;
    assert(pxa_esp_transfer_receipt_store(&config,3,&changed,memory,size,&durable)==PXA_STATUS_BAD_STATE && sync_calls==before);
    read_only=1;
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded));
    assert(pxa_esp_transfer_receipt_store(&config,3,&next,memory,size,&durable)==PXA_STATUS_DENIED);read_only=0;
    key.available=0;
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,&loaded)==PXA_STATUS_UNAVAILABLE && !loaded.transfer.revision);key.available=1;
    receipt=next;assert(!pxa_install_transfer_advance(&receipt.transfer,PXA_INSTALL_TRANSFER_COMMITTED,&next.transfer));
    assert(!pxa_esp_transfer_receipt_store(&config,3,&next,memory,size,&durable) && durable);mirrored(directory);
    changed=next;changed.transfer.phase=PXA_INSTALL_TRANSFER_ABORTED;changed.transfer.revision++;
    assert(pxa_esp_transfer_receipt_store(&config,4,&changed,memory,size,&durable)==PXA_STATUS_BAD_STATE && !durable);
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,(pxa_esp_transfer_receipt_t *)memory)==PXA_STATUS_INVALID_ARGUMENT);
    assert(!pxa_volume_update(registry,source_epoch,PXA_VOLUME_DRAINING,0,0,0));
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,&loaded)==PXA_STATUS_UNAVAILABLE);
    assert(!pxa_volume_update(registry,source_epoch,PXA_VOLUME_READY,0,0,0));
    for(unsigned i=0;i<2;++i) {file=openat(directory,slots[i],O_WRONLY|O_TRUNC);assert(file>=0 && write(file,"PX",2)==2 && !close(file));}
    assert(pxa_esp_transfer_receipt_load(&config,memory,size,&loaded)==PXA_STATUS_DENIED);
    receipt=fixture();assert(pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size,&durable)==PXA_STATUS_DENIED);
    assert(!unlinkat(directory,slots[0],0) && !unlinkat(directory,slots[1],0));
    /* FAT does not rename over an existing name. Check the extra barrier and
     * failure after removal without losing the authoritative peer. */
    no_replace=1;receipt=fixture();
    assert(!pxa_esp_transfer_receipt_store(&config,0,&receipt,memory,size,&durable) && durable);
    assert(!pxa_esp_transfer_receipt_store(&config,1,&receipt,memory,size,&durable) && durable);mirrored(directory);
    next=receipt;assert(!pxa_install_transfer_advance(&receipt.transfer,PXA_INSTALL_TRANSFER_READY,&next.transfer));
    fail_sync_at=sync_calls+2;
    assert(pxa_esp_transfer_receipt_store(&config,1,&next,memory,size,&durable)==PXA_STATUS_IO_ERROR && !durable);
    fail_sync_at=0;
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded) && loaded.transfer.revision==1);
    assert(!pxa_esp_transfer_receipt_store(&config,1,&next,memory,size,&durable) && durable);mirrored(directory);
    receipt=next;assert(!pxa_install_transfer_advance(&receipt.transfer,PXA_INSTALL_TRANSFER_SURRENDERED,&next.transfer));
    fail_publish=1;
    assert(pxa_esp_transfer_receipt_store(&config,2,&next,memory,size,&durable)==PXA_STATUS_IO_ERROR && !durable);
    assert(!pxa_esp_transfer_receipt_load(&config,memory,size,&loaded) && loaded.transfer.revision==2);
    assert(!pxa_esp_transfer_receipt_store(&config,2,&next,memory,size,&durable) && durable);mirrored(directory);
    assert(!unlinkat(directory,slots[0],0) && !unlinkat(directory,slots[1],0) && !close(directory) && !rmdir(path));
    assert(!pxa_volume_unregister(registry,source_epoch) && !pxa_volume_unregister(registry,target_epoch));
    free(volume_memory);free(memory);
    printf("Transfer receipts: device/role/volume/home binding, immutable context/CAS, torn mirror, uncertain sync, read-only/key/drain failures and no false empty; workspace=%zu bytes\n",size);
    return 0;
}
