#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_transfer_receipt.h"
#include "pxa_esp_record_seal.h"
#include "pxa/pxa_esp_storage.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define openat pxa_esp_openat
#define close pxa_esp_close
#define fstat pxa_esp_fstat
#define fsync pxa_esp_fsync
#define renameat pxa_esp_renameat
#define unlinkat pxa_esp_unlinkat
#endif

#define RECEIPT_HEADER 16u
#define RECEIPT_MAX (RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES+PXA_ESP_TRANSFER_CONTEXT_BYTES+32u)
static const char *const slots[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1};
static const uint8_t purpose[]="pxa.install.transfer.v1";
typedef struct {
    uint8_t slots[2][RECEIPT_MAX],packed[RECEIPT_MAX],encoded[PXA_INSTALL_TRANSFER_BYTES];
    size_t bytes[2],packed_size;
    pxa_install_transfer_t decoded,advanced;
    pxa_esp_transfer_receipt_t old;
    pxa_volume_info_t volume;
    uint8_t binding[17+129];char locator[130];
    uint8_t durable_slots;
} receipt_state_t;

size_t pxa_esp_transfer_receipt_workspace_size(void) {return sizeof(receipt_state_t);}
static int overlap(const void *a,size_t an,const void *b,size_t bn) {
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
static pxa_status_t io_error(void) {
    if(errno==ENOSPC || errno==EDQUOT)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOMEM || errno==EMFILE || errno==ENFILE)return PXA_STATUS_RESOURCE_LIMIT;
    if(errno==ELOOP || errno==ENOTDIR)return PXA_STATUS_DENIED;
    return PXA_STATUS_IO_ERROR;
}
static int valid_config(const pxa_esp_transfer_receipt_config_t *config) {
    return config && config->directory>=0 && config->mount_epoch &&
        (config->role==PXA_ESP_TRANSFER_SOURCE || config->role==PXA_ESP_TRANSFER_TARGET) &&
        config->locator && strlen(config->locator)>=66 && strlen(config->locator)<=129;
}
static pxa_status_t available(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,int writing) {
    pxa_status_t status=pxa_esp_storage_volume(config->mount_epoch,&state->volume);
    if(status!=PXA_STATUS_OK || state->volume.state!=PXA_VOLUME_READY || state->volume.ambiguous)return PXA_STATUS_UNAVAILABLE;
    uint64_t required=PXA_VOLUME_READ|(writing?PXA_VOLUME_WRITE:0);
    return (state->volume.capabilities&required)==required?PXA_STATUS_OK:PXA_STATUS_DENIED;
}
static pxa_status_t binding(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,
    const pxa_install_transfer_t *transfer,pxa_bytes_t *output) {
    static const char hex[]="0123456789abcdef";
    const pxa_volume_id_t *volume=config->role==PXA_ESP_TRANSFER_SOURCE?&transfer->source.app_volume:&transfer->target_app_volume;
    if(memcmp(volume->bytes,state->volume.id.bytes,16))return PXA_STATUS_DENIED;
    for(size_t i=0;i<32;++i) {
        state->locator[2*i]=hex[transfer->source.publisher_root[i]>>4];
        state->locator[2*i+1]=hex[transfer->source.publisher_root[i]&15];
    }
    state->locator[64]='/';strcpy(state->locator+65,transfer->source.app_id);
    if(strcmp(state->locator,config->locator))return PXA_STATUS_DENIED;
    state->binding[0]=config->role;memcpy(state->binding+1,state->volume.id.bytes,16);
    size_t n=strlen(config->locator);memcpy(state->binding+17,config->locator,n);
    *output=(pxa_bytes_t){state->binding,17+n};return PXA_STATUS_OK;
}
static pxa_status_t read_slot(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,unsigned slot) {
    uint8_t *p=state->slots[slot];state->bytes[slot]=0;
    pxa_status_t status=available(config,state,0);if(status!=PXA_STATUS_OK)return status;
    int file=openat(config->directory,slots[slot],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(file<0) {
        if(errno==ENOENT)return PXA_STATUS_OK;
        if(errno==ELOOP) {state->bytes[slot]=1;return PXA_STATUS_OK;}
        return io_error();
    }
    struct stat before,after;size_t count=0,expected=0;
    if(fstat(file,&before)!=0)status=io_error();
    else if(!S_ISREG(before.st_mode) || before.st_nlink>1 || before.st_size<(off_t)(RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES+32) ||
        before.st_size>(off_t)RECEIPT_MAX)status=PXA_STATUS_DENIED;
    else expected=(size_t)before.st_size;
    while(status==PXA_STATUS_OK && count<expected) {
        ssize_t n=read(file,p+count,expected-count);
        if(n<0 && errno==EINTR)continue;
        if(n<0) {status=io_error();break;}
        if(!n) {status=PXA_STATUS_DENIED;break;}
        count+=(size_t)n;
    }
    if(status==PXA_STATUS_OK && (fstat(file,&after)!=0 || before.st_size!=after.st_size ||
        before.st_dev!=after.st_dev || before.st_ino!=after.st_ino || before.st_mtime!=after.st_mtime))status=PXA_STATUS_DENIED;
    if(close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK) {
        size_t context=pxa_read_u16(p+10);
        if(memcmp(p,"PXMS",4) || pxa_read_u16(p+4)!=1 || pxa_read_u16(p+6)!=RECEIPT_HEADER ||
            p[8]!=config->role || p[9] || pxa_read_u16(p+12)!=PXA_INSTALL_TRANSFER_BYTES || pxa_read_u16(p+14) ||
            context>PXA_ESP_TRANSFER_CONTEXT_BYTES || count!=RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES+context+32)
            status=PXA_STATUS_DENIED;
    }
    if(status==PXA_STATUS_OK)status=pxa_install_transfer_decode((pxa_bytes_t){p+RECEIPT_HEADER,PXA_INSTALL_TRANSFER_BYTES},NULL,&state->decoded);
    pxa_bytes_t bound={0};
    if(status==PXA_STATUS_OK)status=binding(config,state,&state->decoded,&bound);
    if(status==PXA_STATUS_OK)status=pxa_esp_record_verify((pxa_bytes_t){purpose,sizeof(purpose)-1},bound,
        (pxa_bytes_t){p,count-32},p+count-32);
    if(status==PXA_STATUS_DENIED || status==PXA_STATUS_INVALID_ARGUMENT) {state->bytes[slot]=1;return PXA_STATUS_OK;}
    if(status==PXA_STATUS_OK)state->bytes[slot]=count;
    return status;
}
static pxa_bytes_t core_bytes(receipt_state_t *state,unsigned slot) {
    return (pxa_bytes_t){state->slots[slot]+RECEIPT_HEADER,state->bytes[slot]>1?PXA_INSTALL_TRANSFER_BYTES:state->bytes[slot]};
}
static pxa_status_t load(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,
    pxa_esp_transfer_receipt_t *output,uint8_t *selected) {
    memset(output,0,sizeof(*output));
    pxa_status_t status=read_slot(config,state,0);if(status!=PXA_STATUS_OK)return status;
    status=read_slot(config,state,1);if(status!=PXA_STATUS_OK)return status;
    if(state->bytes[0]>1 && state->bytes[1]>1) {
        size_t first=pxa_read_u16(state->slots[0]+10),second=pxa_read_u16(state->slots[1]+10);
        if(first!=second || memcmp(state->slots[0]+RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES,
            state->slots[1]+RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES,first))return PXA_STATUS_DENIED;
    }
    status=pxa_install_transfer_select(core_bytes(state,0),core_bytes(state,1),NULL,&output->transfer,selected);
    if(status!=PXA_STATUS_OK)return status;
    state->durable_slots=(uint8_t)((state->bytes[0]>1?1:0)|(state->bytes[1]>1?2:0));
    uint8_t *p=state->slots[*selected];output->context_size=pxa_read_u16(p+10);
    memcpy(output->context,p+RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES,output->context_size);
    return available(config,state,0);
}
static pxa_status_t pack(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,
    const pxa_esp_transfer_receipt_t *receipt) {
    if(receipt->context_size>PXA_ESP_TRANSFER_CONTEXT_BYTES)return PXA_STATUS_INVALID_ARGUMENT;
    uint8_t *p=state->packed;memset(p,0,RECEIPT_MAX);
    pxa_status_t status=pxa_install_transfer_encode(&receipt->transfer,p+RECEIPT_HEADER);
    if(status!=PXA_STATUS_OK)return status;
    pxa_bytes_t bound;
    status=available(config,state,1);if(status==PXA_STATUS_OK)status=binding(config,state,&receipt->transfer,&bound);
    if(status!=PXA_STATUS_OK)return status;
    memcpy(p,"PXMS",4);pxa_write_u16(p+4,1);pxa_write_u16(p+6,RECEIPT_HEADER);p[8]=config->role;
    pxa_write_u16(p+10,receipt->context_size);pxa_write_u16(p+12,PXA_INSTALL_TRANSFER_BYTES);
    memcpy(p+RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES,receipt->context,receipt->context_size);
    size_t bytes=RECEIPT_HEADER+PXA_INSTALL_TRANSFER_BYTES+receipt->context_size;
    status=pxa_esp_record_seal((pxa_bytes_t){purpose,sizeof(purpose)-1},bound,(pxa_bytes_t){p,bytes},p+bytes);
    if(status==PXA_STATUS_OK)state->packed_size=bytes+32;
    return status;
}
static pxa_status_t write_slot(const pxa_esp_transfer_receipt_config_t *config,receipt_state_t *state,unsigned slot) {
    pxa_status_t status=available(config,state,1);if(status!=PXA_STATUS_OK)return status;
    char pending[40];snprintf(pending,sizeof(pending),"%s.pending",slots[slot]);
    if(unlinkat(config->directory,pending,0)!=0 && errno!=ENOENT)return io_error();
    int file=openat(config->directory,pending,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(file<0)return io_error();
    size_t count=0;
    while(count<state->packed_size) {
        ssize_t n=write(file,state->packed+count,state->packed_size-count);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {status=io_error();break;}
        count+=(size_t)n;
    }
    if(status==PXA_STATUS_OK && fsync(file)!=0)status=io_error();
    if(close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK)status=available(config,state,1);
    if(status==PXA_STATUS_OK && renameat(config->directory,pending,config->directory,slots[slot])!=0) {
        if(errno==EEXIST || errno==ENOTEMPTY) {
            /* The other slot remains authenticated and durable while FAT's
             * no-replace policy requires retiring this replaceable slot.
             * Persist the gap, then publish the already-synced temporary.
             * Reset in this gap loads the peer, never an empty installation. */
            if(!(state->durable_slots&(1u<<(slot^1u))))status=PXA_STATUS_DENIED;
            if(status==PXA_STATUS_OK) {
                if(unlinkat(config->directory,slots[slot],0)!=0)status=io_error();
                else state->durable_slots&=(uint8_t)~(1u<<slot);
            }
            if(status==PXA_STATUS_OK && fsync(config->directory)!=0)status=io_error();
            if(status==PXA_STATUS_OK)status=available(config,state,1);
            if(status==PXA_STATUS_OK && renameat(config->directory,pending,config->directory,slots[slot])!=0)status=io_error();
        } else status=io_error();
    }
    if(status==PXA_STATUS_OK && fsync(config->directory)!=0)status=io_error();
    if(status==PXA_STATUS_OK)state->durable_slots|=(uint8_t)(1u<<slot);
    if(status!=PXA_STATUS_OK)(void)unlinkat(config->directory,pending,0);
    return status;
}
static pxa_status_t workspace(const pxa_esp_transfer_receipt_config_t *config,void *memory,size_t bytes) {
    if(!valid_config(config) || !memory || (uintptr_t)memory%sizeof(uint64_t))return PXA_STATUS_INVALID_ARGUMENT;
    if(overlap(config,sizeof(*config),memory,sizeof(receipt_state_t)) ||
        overlap(config->locator,strlen(config->locator)+1,memory,sizeof(receipt_state_t)))return PXA_STATUS_INVALID_ARGUMENT;
    return bytes<sizeof(receipt_state_t)?PXA_STATUS_RESOURCE_LIMIT:PXA_STATUS_OK;
}
pxa_status_t pxa_esp_transfer_receipt_load(const pxa_esp_transfer_receipt_config_t *config,
    void *memory,size_t bytes,pxa_esp_transfer_receipt_t *output) {
    if(!output || (memory && overlap(memory,sizeof(receipt_state_t),output,sizeof(*output))))return PXA_STATUS_INVALID_ARGUMENT;
    memset(output,0,sizeof(*output));pxa_status_t status=workspace(config,memory,bytes);if(status!=PXA_STATUS_OK)return status;
    receipt_state_t *state=memory;memset(state,0,sizeof(*state));uint8_t selected;
    status=load(config,state,output,&selected);
    if(status!=PXA_STATUS_OK)memset(output,0,sizeof(*output));
    return status;
}
pxa_status_t pxa_esp_transfer_receipt_store(const pxa_esp_transfer_receipt_config_t *config,
    uint64_t expected,const pxa_esp_transfer_receipt_t *receipt,void *memory,size_t bytes,uint8_t *durable) {
    if(!receipt || !durable || (memory && (overlap(memory,sizeof(receipt_state_t),receipt,sizeof(*receipt)) ||
        overlap(memory,sizeof(receipt_state_t),durable,1))) || overlap(receipt,sizeof(*receipt),durable,1))return PXA_STATUS_INVALID_ARGUMENT;
    *durable=0;pxa_status_t status=workspace(config,memory,bytes);if(status!=PXA_STATUS_OK)return status;
    receipt_state_t *state=memory;memset(state,0,sizeof(*state));uint8_t selected=1;
    status=load(config,state,&state->old,&selected);
    if(status==PXA_STATUS_NOT_FOUND) {
        if(expected || receipt->transfer.revision!=1 || receipt->transfer.phase!=PXA_INSTALL_TRANSFER_COPYING)return PXA_STATUS_BAD_STATE;
    } else if(status!=PXA_STATUS_OK)return status;
    else {
        if(state->old.transfer.revision!=expected || state->old.context_size!=receipt->context_size ||
            memcmp(state->old.context,receipt->context,receipt->context_size))return PXA_STATUS_BAD_STATE;
        status=pxa_install_transfer_advance(&state->old.transfer,receipt->transfer.phase,&state->advanced);
        if(status!=PXA_STATUS_OK)return status;
        status=pxa_install_transfer_encode(&state->advanced,state->encoded);if(status!=PXA_STATUS_OK)return status;
        status=pack(config,state,receipt);if(status!=PXA_STATUS_OK)return status;
        if(memcmp(state->encoded,state->packed+RECEIPT_HEADER,PXA_INSTALL_TRANSFER_BYTES))return PXA_STATUS_BAD_STATE;
        if(receipt->transfer.revision==expected) {
            *durable=1;return write_slot(config,state,selected^1u);
        }
    }
    status=pack(config,state,receipt);if(status!=PXA_STATUS_OK)return status;
    status=write_slot(config,state,selected^1u);if(status!=PXA_STATUS_OK)return status;
    *durable=1;return write_slot(config,state,selected);
}
