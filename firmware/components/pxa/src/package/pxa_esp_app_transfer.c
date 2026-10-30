#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_app_transfer.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_installations_internal.h"
#include "pxa_esp_transfer_receipt.h"
#include "pxa_esp_private_copy.h"
#include "pxa_esp_private_authority.h"
#include "pxa_esp_volumes.h"
#include "pxa_esp_publisher_directory.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/openssl/pxa_openssl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define openat pxa_esp_openat
#define fstat pxa_esp_fstat
#define close pxa_esp_close
#define fstatat pxa_esp_fstatat
#define mkdirat pxa_esp_mkdirat
#define renameat pxa_esp_renameat
#define unlinkat pxa_esp_unlinkat
#define fsync pxa_esp_fsync
#define dup pxa_esp_dup
#define fdopendir pxa_esp_fdopendir
#endif

typedef struct {
    pxa_esp_installations_t *source,*target;
    pxa_esp_app_storage_t *source_storage,*target_storage;
    pxa_esp_transfer_receipt_t receipt,other;
    pxa_esp_transfer_receipt_config_t source_receipt,target_receipt;
    pxa_posix_app_home_t original,candidate,checked;
    pxa_install_record_t projected,seed;
    pxa_install_plan_t plan;
    pxa_esp_private_copy_info_t private_info;
    pxa_esp_private_copy_config_t copy_config;
    const pxa_esp_app_transfer_control_t *control;
    void *receipt_workspace,*copy_workspace;
    char locator[PXA_POSIX_APP_LOCATOR_BYTES],nonce[33],source_package[512],retired[512],scratch[512];
    uint8_t record_bytes[PXA_POSIX_INSTALL_RECORD_SEALED_BYTES],encoded[PXA_INSTALL_TRANSFER_BYTES];
    int source_parent,source_intent,target_directory;
    uint8_t source_retired,prepared;
    const char *stage;
    pxa_posix_installer_result_t owned_result;
    pxa_package_manifest_t *owned_manifest;
    void *owned_manifest_workspace;
    uint8_t *owned_encoded;
    char owned_root[PXA_POSIX_APP_PATH_BYTES];
    struct stat manifest_before,manifest_after;
} transfer_t;
/* Unmount failure keeps its owner reachable and blocks a subsequent move.
 * Calls are serialized by the Host's catalog transaction lock. */
static transfer_t *pending_close;

static pxa_status_t io_error(void) {
    if(errno==ENOENT)return PXA_STATUS_NOT_FOUND;
    if(errno==ENOSPC || errno==EDQUOT)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOMEM || errno==EMFILE || errno==ENFILE)return PXA_STATUS_RESOURCE_LIMIT;
    if(errno==ELOOP || errno==ENOTDIR)return PXA_STATUS_DENIED;
    return PXA_STATUS_IO_ERROR;
}
static int child(int parent,const char *name,int create) {
    if(create) {
        int made=mkdirat(parent,name,0700);
        if(made!=0 && errno!=EEXIST)return -1;
        if(made==0 && fsync(parent)!=0)return -1;
    }
    return openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static pxa_status_t home_directory(pxa_esp_installations_t *owner,const char *locator,
    int create,int *parent,int *directory) {
    char publisher[65],physical[PXA_POSIX_APP_LOCATOR_BYTES];*parent=-1;*directory=-1;
    pxa_status_t mapped=pxa_esp_publisher_map(owner->root,&owner->volume.id,"apps",locator,0,physical,sizeof(physical));
    if(mapped!=PXA_STATUS_OK)return mapped;
    const char *slash=strchr(physical,'/');
    memcpy(publisher,physical,(size_t)(slash-physical));publisher[slash-physical]=0;
    int root=open(owner->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return io_error();
    int apps=child(root,"apps",create);pxa_status_t status=apps<0?io_error():PXA_STATUS_OK;
    if(close(root)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK) {*parent=child(apps,publisher,create);if(*parent<0)status=io_error();}
    if(apps>=0 && close(apps)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK) {*directory=child(*parent,slash+1,create);if(*directory<0)status=io_error();}
    return status;
}
static pxa_status_t tag(void *context,pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t output[32]) {
    pxa_esp_installations_t *owner=context;
    return owner->config.record_tag(owner->config.record_auth_context,purpose,binding,body,output);
}
static pxa_posix_install_record_auth_t record_auth(transfer_t *move,pxa_esp_installations_t *owner) {
    return (pxa_posix_install_record_auth_t){owner,
        {(const uint8_t *)move->locator,strlen(move->locator)},tag};
}
static int same_record(transfer_t *move,const pxa_install_record_t *a,const pxa_install_record_t *b) {
    uint8_t first[PXA_INSTALL_RECORD_BYTES];
    return pxa_install_record_encode(a,first)==PXA_STATUS_OK &&
        pxa_install_record_encode(b,move->record_bytes)==PXA_STATUS_OK && !memcmp(first,move->record_bytes,sizeof(first));
}
static pxa_status_t available(transfer_t *move) {
    if(move->control && move->control->progress)move->control->progress(move->control->context,0);
    pxa_volume_info_t volume;
    pxa_status_t status=pxa_esp_storage_volume(move->target->volume.mount_epoch,&volume);
    if(status!=PXA_STATUS_OK || volume.state!=PXA_VOLUME_READY || volume.ambiguous)return PXA_STATUS_UNAVAILABLE;
    if(move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED) {
        if(!move->source)return PXA_STATUS_UNAVAILABLE;
        status=pxa_esp_storage_volume(move->source->volume.mount_epoch,&volume);
        if(status!=PXA_STATUS_OK || volume.state!=PXA_VOLUME_READY || volume.ambiguous)return PXA_STATUS_UNAVAILABLE;
        if(move->control && move->control->check)return move->control->check(move->control->context);
    }
    return PXA_STATUS_OK;
}
static pxa_status_t copy_check(void *context) {return available(context);}
static void nonce_text(transfer_t *move) {
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<16;++i) {move->nonce[2*i]=hex[move->receipt.transfer.transfer_id[i]>>4];
        move->nonce[2*i+1]=hex[move->receipt.transfer.transfer_id[i]&15];}
    move->nonce[32]=0;
}
static pxa_status_t source_intent(transfer_t *move,int create) {
    int root=open(move->source->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return io_error();
    int apps=child(root,"apps",0),transfers=-1;
    pxa_status_t status=apps<0?io_error():PXA_STATUS_OK;
    if(close(root)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK) {transfers=child(apps,".transfers",create);if(transfers<0)status=io_error();}
    if(apps>=0 && close(apps)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK) {move->source_intent=child(transfers,move->nonce,create);if(move->source_intent<0)status=io_error();}
    if(transfers>=0 && close(transfers)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status!=PXA_STATUS_OK)return status;
    move->source_receipt=(pxa_esp_transfer_receipt_config_t){move->source_intent,
        move->source->volume.mount_epoch,PXA_ESP_TRANSFER_SOURCE,move->locator};
    int n=snprintf(move->retired,sizeof(move->retired),"%s/apps/.transfers/%s/source",move->source->root,move->nonce);
    return n<0 || (size_t)n>=sizeof(move->retired)?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
static int same_transfer(transfer_t *move,const pxa_esp_transfer_receipt_t *other) {
    pxa_install_transfer_t normalized=other->transfer;
    normalized.revision=move->receipt.transfer.revision;normalized.phase=move->receipt.transfer.phase;
    if(other->context_size!=move->receipt.context_size || memcmp(other->context,move->receipt.context,other->context_size) ||
        pxa_install_transfer_encode(&move->receipt.transfer,move->encoded)!=PXA_STATUS_OK ||
        pxa_install_transfer_encode(&normalized,(uint8_t *)move->scratch)!=PXA_STATUS_OK)return 0;
    return !memcmp(move->encoded,move->scratch,PXA_INSTALL_TRANSFER_BYTES);
}
static pxa_status_t persist_phase(transfer_t *move,const pxa_esp_transfer_receipt_config_t *endpoint,uint8_t phase) {
    pxa_status_t status=pxa_esp_transfer_receipt_load(endpoint,move->receipt_workspace,
        pxa_esp_transfer_receipt_workspace_size(),&move->other);
    uint64_t expected=0;uint8_t durable=0;
    if(status==PXA_STATUS_NOT_FOUND) {
        move->other=move->receipt;move->other.transfer.revision=1;move->other.transfer.phase=PXA_INSTALL_TRANSFER_COPYING;
        status=pxa_esp_transfer_receipt_store(endpoint,0,&move->other,move->receipt_workspace,
            pxa_esp_transfer_receipt_workspace_size(),&durable);
    } else if(status==PXA_STATUS_OK && !same_transfer(move,&move->other))return PXA_STATUS_DENIED;
    if(status!=PXA_STATUS_OK)return status;
    if(move->other.transfer.phase==PXA_INSTALL_TRANSFER_ABORTED)return PXA_STATUS_BAD_STATE;
    if(move->other.transfer.phase>phase)return PXA_STATUS_OK;
    while(move->other.transfer.phase<phase) {
        expected=move->other.transfer.revision;
        status=pxa_install_transfer_advance(&move->other.transfer,move->other.transfer.phase+1,&move->other.transfer);
        if(status==PXA_STATUS_OK)status=pxa_esp_transfer_receipt_store(endpoint,expected,&move->other,
            move->receipt_workspace,pxa_esp_transfer_receipt_workspace_size(),&durable);
        if(status!=PXA_STATUS_OK)return status;
    }
    return pxa_esp_transfer_receipt_store(endpoint,move->other.transfer.revision,&move->other,
        move->receipt_workspace,pxa_esp_transfer_receipt_workspace_size(),&durable);
}
static pxa_status_t source_location(transfer_t *move) {
    int parent=-1,canonical=-1;struct stat hidden;
    pxa_status_t status=home_directory(move->source,move->locator,0,&parent,&canonical);
    if(move->source_parent>=0)close(move->source_parent);
    move->source_parent=parent;
    int have_hidden=fstatat(move->source_intent,"source",&hidden,AT_SYMLINK_NOFOLLOW)==0;
    if(!have_hidden && errno!=ENOENT) {if(canonical>=0)close(canonical);return io_error();}
    if(canonical>=0 && have_hidden) {close(canonical);return PXA_STATUS_DENIED;}
    int directory=canonical;
    if(have_hidden) {
        if(status!=PXA_STATUS_NOT_FOUND || !S_ISDIR(hidden.st_mode))return PXA_STATUS_DENIED;
        directory=child(move->source_intent,"source",0);if(directory<0)return io_error();
    } else if(status!=PXA_STATUS_OK)return status;
    pxa_posix_install_record_auth_t auth=record_auth(move,move->source);
    status=pxa_posix_install_record_load_with_auth(directory,&move->source->volume.id,&auth,&move->checked.record);
    if(close(directory)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK && !same_record(move,&move->checked.record,&move->receipt.transfer.source))status=PXA_STATUS_BAD_STATE;
    if(status==PXA_STATUS_OK) {
        status=pxa_esp_transfer_receipt_load(&move->source_receipt,move->receipt_workspace,
            pxa_esp_transfer_receipt_workspace_size(),&move->other);
        if(status==PXA_STATUS_OK && (!same_transfer(move,&move->other) ||
            (!have_hidden && move->other.transfer.phase>=PXA_INSTALL_TRANSFER_SURRENDERED)))status=PXA_STATUS_DENIED;
    }
    if(status==PXA_STATUS_OK)move->source_retired=(uint8_t)have_hidden;
    return status;
}
static pxa_status_t close_private(transfer_t *move) {
    pxa_status_t status=pxa_esp_app_storage_close(&move->source_storage);
    pxa_status_t other=pxa_esp_app_storage_close(&move->target_storage);
    return status==PXA_STATUS_OK?other:status;
}
static pxa_status_t measure_source(transfer_t *move) {
    move->stage="measure-private";
    const char *root;
    pxa_status_t status=pxa_esp_app_storage_create(&move->original,move->source->volume.mount_epoch,
        pxa_esp_installations_private_capacity(),NULL,&move->source_storage);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(move->source_storage,&root);
    if(status==PXA_STATUS_OK)status=pxa_esp_private_copy_measure_install(root,&move->copy_config,move->copy_workspace,
        pxa_esp_private_copy_workspace_size(),&move->private_info);
    pxa_status_t closed=close_private(move);return status==PXA_STATUS_OK?closed:status;
}
static pxa_status_t prepare_target(void *context,const pxa_posix_app_home_t *home) {
    transfer_t *move=context;const char *source_root,*target_root;
    move->stage="prepare-private";
    if(!same_record(move,&home->record,&move->projected))return PXA_STATUS_DENIED;
    pxa_status_t status=available(move);if(status!=PXA_STATUS_OK)return status;
    if(move->prepared || move->receipt.transfer.phase>=PXA_INSTALL_TRANSFER_SURRENDERED || move->source_retired) {
        status=pxa_esp_app_storage_create(home,move->target->volume.mount_epoch,
            pxa_esp_installations_private_capacity(),&move->plan,&move->target_storage);
        if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(move->target_storage,&target_root);
        pxa_status_t closed=close_private(move);return status==PXA_STATUS_OK?closed:status;
    }
    status=pxa_posix_app_home_path(home,PXA_POSIX_APP_HOME_PRIVATE,move->scratch,sizeof(move->scratch));
    if(status==PXA_STATUS_OK) {
        status=pxa_posix_fs_remove_tree(move->scratch);if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;
    }
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_create(&move->original,move->source->volume.mount_epoch,
        pxa_esp_installations_private_capacity(),NULL,&move->source_storage);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(move->source_storage,&source_root);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_create(home,move->target->volume.mount_epoch,
        pxa_esp_installations_private_capacity(),&move->plan,&move->target_storage);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_root(move->target_storage,&target_root);
    if(status==PXA_STATUS_OK)status=pxa_esp_private_copy_tree(source_root,target_root,&move->copy_config,
        move->copy_workspace,pxa_esp_private_copy_workspace_size(),&move->private_info);
    if(status==PXA_STATUS_OK) {move->stage="transfer-authority";status=pxa_esp_private_authority_copy(move->source_storage,move->target_storage);}
    pxa_status_t closed=close_private(move);if(status==PXA_STATUS_OK)status=closed;
    if(status==PXA_STATUS_OK)move->prepared=1;
    return status;
}
static pxa_status_t handoff(void *context,const pxa_posix_app_home_t *home) {
    transfer_t *move=context;
    move->stage="source-handoff";
    if(!same_record(move,&home->record,&move->projected))return PXA_STATUS_DENIED;
    if(move->receipt.transfer.phase>=PXA_INSTALL_TRANSFER_SURRENDERED)return available(move);
    pxa_status_t status=available(move);if(status!=PXA_STATUS_OK)return status;
    status=persist_phase(move,&move->target_receipt,PXA_INSTALL_TRANSFER_READY);
    if(status==PXA_STATUS_OK)status=persist_phase(move,&move->source_receipt,PXA_INSTALL_TRANSFER_READY);
    if(status==PXA_STATUS_OK)status=source_location(move);
    if(status!=PXA_STATUS_OK)return status;
    if(!move->source_retired) {
        /* Private mounts are closed before the canonical home is renamed. */
        status=pxa_esp_volumes_bind_app_home(move->original.home,move->source->volume.mount_epoch);
        if(status!=PXA_STATUS_OK)return status;
        status=pxa_esp_volumes_unbind_private(move->original.home,move->source->volume.mount_epoch);
        if(status!=PXA_STATUS_OK)return status;
        if(renameat(move->source_parent,strchr(move->locator,'/')+1,move->source_intent,"source")!=0)return io_error();
        move->source_retired=1;
    }
    if(fsync(move->source_parent)!=0 || fsync(move->source_intent)!=0)return io_error();
    status=persist_phase(move,&move->source_receipt,PXA_INSTALL_TRANSFER_SURRENDERED);
    if(status==PXA_STATUS_OK)status=persist_phase(move,&move->target_receipt,PXA_INSTALL_TRANSFER_SURRENDERED);
    if(status==PXA_STATUS_OK)status=pxa_esp_transfer_receipt_load(&move->target_receipt,move->receipt_workspace,
        pxa_esp_transfer_receipt_workspace_size(),&move->receipt);
    return status;
}
static pxa_status_t seed_target(transfer_t *move) {
    move->seed=move->projected;move->seed.generation=move->receipt.transfer.source.generation;
    move->seed.flags|=PXA_INSTALL_RECORD_ARCHIVED;
    pxa_posix_install_record_auth_t auth=record_auth(move,move->target);
    pxa_status_t status=pxa_posix_install_record_load_with_auth(move->target_directory,&move->target->volume.id,&auth,&move->checked.record);
    if(status==PXA_STATUS_OK) {
        if(!same_record(move,&move->seed,&move->checked.record) && !same_record(move,&move->projected,&move->checked.record))
            return PXA_STATUS_DENIED;
        if(same_record(move,&move->projected,&move->checked.record) &&
            move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED)return PXA_STATUS_DENIED;
        return pxa_posix_install_record_mirror(move->target_directory,&move->checked.record,&auth);
    }
    if(status!=PXA_STATUS_NOT_FOUND)return status;
    size_t size=0;status=pxa_posix_install_record_pack(&move->seed,&auth,move->record_bytes,&size);
    if(status!=PXA_STATUS_OK)return status;
    /* Only this receipt-owned unpublished home can bootstrap at generation N.
     * Ordinary new installs still start at one, preserving stale-CAS safety. */
    if(unlinkat(move->target_directory,"install.seed.pending",0)!=0 && errno!=ENOENT)return io_error();
    int file=openat(move->target_directory,"install.seed.pending",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(file<0)return io_error();
    size_t offset=0;
    while(offset<size) {
        ssize_t n=write(file,move->record_bytes+offset,size-offset);if(n<0 && errno==EINTR)continue;
        if(n<=0) {status=io_error();break;}offset+=(size_t)n;
    }
    if(status==PXA_STATUS_OK && fsync(file)!=0)status=io_error();
    if(close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK && renameat(move->target_directory,"install.seed.pending",move->target_directory,"install.0.pxi")!=0)
        status=io_error();
    if(status==PXA_STATUS_OK && fsync(move->target_directory)!=0)status=io_error();
    if(status==PXA_STATUS_OK)status=pxa_posix_install_record_mirror(move->target_directory,&move->seed,&auth);
    return status;
}
static pxa_status_t namespace_credit(transfer_t *move,uint64_t *credit) {
    *credit=0;
    int root=open(move->target->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return io_error();
    int apps=child(root,"apps",0);int saved=errno;close(root);errno=saved;
    if(apps<0)return errno==ENOENT?PXA_STATUS_OK:io_error();
    *credit+=move->target->volume.allocation_unit;
    char physical[PXA_POSIX_APP_LOCATOR_BYTES];
    pxa_status_t mapped=pxa_esp_publisher_map(move->target->root,&move->target->volume.id,"apps",move->locator,0,physical,sizeof(physical));
    if(mapped!=PXA_STATUS_OK) {close(apps);return mapped;}
    *strchr(physical,'/')=0;const char *name=physical;
    int publisher=child(apps,name,0);saved=errno;close(apps);errno=saved;
    if(publisher<0)return errno==ENOENT?PXA_STATUS_OK:io_error();
    *credit+=move->target->volume.allocation_unit;
    if(close(publisher)!=0)return io_error();
    if(move->target_directory>=0)*credit+=move->target->volume.allocation_unit;
    return PXA_STATUS_OK;
}
static pxa_status_t plan_target(transfer_t *move,const pxa_package_manifest_t *manifest,uint8_t reserve) {
    if(move->plan.reserved)return PXA_STATUS_OK;
    uint8_t copying=move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED;
    pxa_status_t status=manifest?pxa_esp_installations_plan(move->target,manifest,0,copying,&move->plan):
        pxa_esp_installations_plan_archive(move->target,copying,&move->plan);
    if(status!=PXA_STATUS_OK)return status;
    /* Two receipt slots plus the slot being atomically replaced. Bootstrap
     * has one sealed seed temporary; ordinary install records/directories
     * are already covered by installations_plan(). No container copy. */
    uint64_t target_receipt,source_receipt,seed_bytes;
    const uint64_t receipt_bytes=16u+PXA_INSTALL_TRANSFER_BYTES+32u; /* No backend context in this coordinator. */
    status=pxa_install_file_allocation(&move->target->volume,receipt_bytes,&target_receipt);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&move->source->volume,receipt_bytes,&source_receipt);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&move->target->volume,
        PXA_POSIX_INSTALL_RECORD_SEALED_BYTES,&seed_bytes);
    if(status!=PXA_STATUS_OK)return status;
    uint64_t extra=3*target_receipt+seed_bytes;
    if(copying && (move->target->volume.capabilities&PXA_VOLUME_PRIVATE_NATIVE)) {
        if(move->private_info.allocated_bytes>UINT64_MAX-extra)return PXA_STATUS_LIMIT_EXCEEDED;
        extra+=move->private_info.allocated_bytes;
    }
    if(move->plan.volumes[0].bytes>UINT64_MAX-extra)return PXA_STATUS_LIMIT_EXCEEDED;
    uint64_t app_bytes=move->plan.volumes[0].bytes+extra;
    uint64_t existing=0;
    status=namespace_credit(move,&existing);if(status!=PXA_STATUS_OK)return status;
    if(move->target_directory>=0) {
        /* A resumed receipt-owned canonical home already has apps/publisher/
         * home and its current receipt slots. Free space already excludes
         * them; charge only their future replacement peak, not a second copy
         * of the same committed namespace. Keep the normal metadata margin. */
        const char *slots[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1};
        for(unsigned i=0;i<2;++i) {
            struct stat metadata;
            if(fstatat(move->target_directory,slots[i],&metadata,AT_SYMLINK_NOFOLLOW)!=0) {
                if(errno==ENOENT)continue;
                return io_error();
            }
            if(S_ISREG(metadata.st_mode) && metadata.st_nlink<=1 && metadata.st_size==(off_t)receipt_bytes)
                existing+=target_receipt;
        }
    }
    if(existing>=app_bytes)return PXA_STATUS_DENIED;
    app_bytes-=existing;
    uint64_t source_bytes=move->source_intent>=0?source_receipt:
        3*source_receipt+UINT64_C(2)*move->source->volume.allocation_unit;
    /* The app plan already includes its headroom. Add source headroom once;
     * re-evaluating the joint lease must not charge the app a second time. */
    pxa_install_requirements_t requirements={app_bytes,0,source_bytes+4096,0,0};
    status=pxa_esp_storage_plan(&requirements,move->target->volume.mount_epoch,
        move->source->volume.mount_epoch,&move->plan);
    return status==PXA_STATUS_OK && reserve?pxa_esp_storage_reserve_plan(&move->plan):status;
}
static pxa_status_t reset_unpublished(transfer_t *move) {
    move->stage="reset-unpublished";
    if(move->source_retired || move->receipt.transfer.phase>=PXA_INSTALL_TRANSFER_SURRENDERED)return PXA_STATUS_DENIED;
    move->seed=move->projected;move->seed.generation=move->receipt.transfer.source.generation;
    move->seed.flags|=PXA_INSTALL_RECORD_ARCHIVED;
    pxa_posix_install_record_auth_t auth=record_auth(move,move->target);
    pxa_status_t status=pxa_posix_install_record_load_with_auth(move->target_directory,&move->target->volume.id,&auth,&move->checked.record);
    if(status==PXA_STATUS_OK && !same_record(move,&move->seed,&move->checked.record))return PXA_STATUS_DENIED;
    if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return status;
    pxa_status_t mapped=pxa_esp_publisher_path(move->target->volume.mount_epoch,"apps",move->locator,move->candidate.home,sizeof(move->candidate.home));
    if(mapped!=PXA_STATUS_OK)return mapped;
    int n;DIR *entries=opendir(move->candidate.home);if(!entries)return io_error();
    status=PXA_STATUS_OK;
    for(;;) {
        errno=0;struct dirent *entry=readdir(entries);
        if(!entry) {if(errno)status=io_error();break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..") ||
            !strcmp(entry->d_name,PXA_ESP_TRANSFER_SLOT_0) || !strcmp(entry->d_name,PXA_ESP_TRANSFER_SLOT_1))continue;
        struct stat metadata;
        if(fstatat(move->target_directory,entry->d_name,&metadata,AT_SYMLINK_NOFOLLOW)!=0) {status=io_error();break;}
        if(S_ISDIR(metadata.st_mode)) {
            n=snprintf(move->scratch,sizeof(move->scratch),"%s/%s",move->candidate.home,entry->d_name);
            if(n<0 || (size_t)n>=sizeof(move->scratch)) {status=PXA_STATUS_LIMIT_EXCEEDED;break;}
            status=pxa_posix_fs_remove_tree(move->scratch);
        } else if(unlinkat(move->target_directory,entry->d_name,0)!=0)status=io_error();
        if(status!=PXA_STATUS_OK)break;
    }
    if(closedir(entries)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK && fsync(move->target_directory)!=0)status=io_error();
    return status;
}
static int manifest_in_home(int home) {
    int package=child(home,"package",0);if(package<0)return -1;
    int file=openat(package,"manifest.pxm",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int saved=errno;close(package);errno=saved;return file;
}
static pxa_status_t owned_result(transfer_t *move,pxa_posix_installer_result_t **result) {
    if(*result)return PXA_STATUS_OK;
    move->stage="read-manifest";int file=-1;
    if(move->target_directory>=0)file=manifest_in_home(move->target_directory);
    if(file<0 && move->target_directory>=0 && errno!=ENOENT)return io_error();
    if(file<0 && move->source_intent>=0) {
        int hidden=child(move->source_intent,"source",0);
        if(hidden>=0) {file=manifest_in_home(hidden);int saved=errno;close(hidden);errno=saved;}
        if(file<0 && errno!=ENOENT)return io_error();
    }
    if(file<0 && move->source) {
        int parent=-1,home=-1;
        pxa_status_t status=home_directory(move->source,move->locator,0,&parent,&home);
        if(parent>=0)close(parent);
        if(status!=PXA_STATUS_OK)return status;
        file=manifest_in_home(home);int saved=errno;close(home);errno=saved;
    }
    if(file<0)return move->source?io_error():PXA_STATUS_UNAVAILABLE;
    pxa_status_t status=PXA_STATUS_OK;struct stat *before=&move->manifest_before,*after=&move->manifest_after;
    if(fstat(file,before)!=0)status=io_error();
    else if(!S_ISREG(before->st_mode) || before->st_nlink>1 || before->st_size<=0 ||
        (uint64_t)before->st_size>SIZE_MAX)status=PXA_STATUS_DENIED;
    else if((uint64_t)before->st_size>PXA_ESP_SIGNED_MANIFEST_MAX_BYTES)status=PXA_STATUS_LIMIT_EXCEEDED;
    size_t size=status==PXA_STATUS_OK?(size_t)before->st_size:0,offset=0;
    if(status==PXA_STATUS_OK) {
        move->owned_encoded=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!move->owned_encoded)status=PXA_STATUS_RESOURCE_LIMIT;
    }
    while(status==PXA_STATUS_OK && offset<size) {
        ssize_t count=read(file,move->owned_encoded+offset,size-offset);
        if(count<0 && errno==EINTR)continue;
        if(count<=0) {status=PXA_STATUS_IO_ERROR;break;}
        offset+=(size_t)count;
    }
    if(status==PXA_STATUS_OK && (fstat(file,after)!=0 || before->st_size!=after->st_size ||
        before->st_ino!=after->st_ino || before->st_dev!=after->st_dev || before->st_mtime!=after->st_mtime))status=PXA_STATUS_DENIED;
    if(close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status!=PXA_STATUS_OK)return status;
    pxa_package_limits_t actual;
    status=pxa_package_manifest_measure((pxa_bytes_t){move->owned_encoded,size},&move->target->config.installer.limits,&actual);
    if(status!=PXA_STATUS_OK)return status;
    size_t bytes=pxa_package_manifest_workspace_size(&actual);
    if(!bytes)return PXA_STATUS_DENIED;
    move->owned_manifest_workspace=heap_caps_aligned_calloc(8,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move->owned_manifest_workspace)return PXA_STATUS_RESOURCE_LIMIT;
    move->owned_result=(pxa_posix_installer_result_t){0};move->owned_result.struct_size=sizeof(move->owned_result);
    move->owned_result.manifest=&move->owned_manifest;move->owned_result.encoded=move->owned_encoded;
    move->owned_result.encoded_capacity=size;move->owned_result.manifest_workspace=move->owned_manifest_workspace;
    move->owned_result.manifest_workspace_size=bytes;move->owned_result.root=move->owned_root;
    move->owned_result.root_capacity=sizeof(move->owned_root);*result=&move->owned_result;return PXA_STATUS_OK;
}
static pxa_status_t inspect_source(transfer_t *move,pxa_posix_installer_result_t *result) {
    pxa_status_t status=pxa_esp_installations_inspect(move->source,move->source_package,result);
    if(status==PXA_STATUS_OK)status=pxa_openssl_sha256(result->encoded,(*result->manifest)->encoded.size,move->record_bytes);
    if(status==PXA_STATUS_OK && memcmp(move->record_bytes,move->receipt.transfer.source.manifest_sha256,32))status=PXA_STATUS_DENIED;
    return status;
}
static pxa_status_t finish_target(transfer_t *move,pxa_posix_app_home_t *home) {
    if(!same_record(move,&home->record,&move->projected) || move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED)
        return PXA_STATUS_BAD_STATE;
    /* Recovery of an already committed record skips prepare_home in the SDK.
     * Still require its prepared private filesystem before removing quarantine. */
    pxa_status_t status=prepare_target(move,home);if(status!=PXA_STATUS_OK)return status;
    pxa_posix_install_record_auth_t auth=record_auth(move,move->target);
    status=pxa_posix_install_record_mirror(move->target_directory,&home->record,&auth);
    if(status==PXA_STATUS_OK)status=persist_phase(move,&move->target_receipt,PXA_INSTALL_TRANSFER_COMMITTED);
    if(status!=PXA_STATUS_OK)return status;
    /* Old state is collectible only after both target records and its local
     * COMMITTED receipt are durable. Offline source cleanup may wait. */
    if(move->source && move->source_intent>=0) {
        status=persist_phase(move,&move->source_receipt,PXA_INSTALL_TRANSFER_COMMITTED);
        if(status==PXA_STATUS_OK)status=pxa_posix_fs_remove_tree(move->retired);
        if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return status;
    }
    const char *names[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1,
        PXA_ESP_TRANSFER_SLOT_0 ".pending",PXA_ESP_TRANSFER_SLOT_1 ".pending"};
    for(unsigned i=0;i<4;++i)
        if(unlinkat(move->target_directory,names[i],0)!=0 && errno!=ENOENT)return io_error();
    if(fsync(move->target_directory)!=0)return io_error();
    if(move->source && move->source_intent>=0) {
        if(close(move->source_intent)!=0)return io_error();
        move->source_intent=-1;
        int n=snprintf(move->scratch,sizeof(move->scratch),"%s/apps/.transfers/%s",move->source->root,move->nonce);
        if(n<0 || (size_t)n>=sizeof(move->scratch))return PXA_STATUS_LIMIT_EXCEEDED;
        status=pxa_posix_fs_remove_tree(move->scratch);
        if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return status;
    }
    return PXA_STATUS_OK;
}
static pxa_status_t run_archive(transfer_t *move,pxa_posix_app_home_t *home) {
    /* An authenticated archive carries private-state authority, never code
     * execution authority. Use the same receipt/CAS handoff without making
     * an installer or a synthetic manifest for an absent package. */
    pxa_status_t mapped=pxa_esp_publisher_path(move->target->volume.mount_epoch,"apps",move->locator,move->candidate.home,sizeof(move->candidate.home));
    if(mapped!=PXA_STATUS_OK)return mapped;
    strcpy(move->candidate.locator,move->locator);move->candidate.record=move->projected;
    pxa_status_t status=available(move);if(status!=PXA_STATUS_OK)return status;
    if(move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED) {
        status=source_location(move);if(status!=PXA_STATUS_OK)return status;
        if(move->source_retired)move->prepared=1;
        else {
            if(!move->plan.reserved) {
                status=measure_source(move);
                if(status==PXA_STATUS_OK)status=reset_unpublished(move);
                if(status==PXA_STATUS_OK)status=plan_target(move,NULL,1);
            }
            if(status==PXA_STATUS_OK)status=seed_target(move);
            if(status==PXA_STATUS_OK)status=prepare_target(move,&move->candidate);
            if(status!=PXA_STATUS_OK)return status;
        }
        status=handoff(move,&move->candidate);if(status!=PXA_STATUS_OK)return status;
    }
    status=seed_target(move);
    if(status==PXA_STATUS_OK)status=prepare_target(move,&move->candidate);
    if(status!=PXA_STATUS_OK)return status;
    pxa_posix_install_record_auth_t auth=record_auth(move,move->target);
    move->stage="commit-archive";
    status=pxa_posix_install_record_load_with_auth(move->target_directory,&move->target->volume.id,&auth,&move->checked.record);
    if(status==PXA_STATUS_OK && !same_record(move,&move->checked.record,&move->projected))
        status=pxa_posix_install_record_commit_with_auth(move->target_directory,
            move->receipt.transfer.source.generation,&move->projected,&auth);
    if(status==PXA_STATUS_OK) {*home=move->candidate;status=finish_target(move,home);}
    return status;
}
static pxa_status_t run(transfer_t *move,pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    move->stage="project-record";
    pxa_status_t status=pxa_install_transfer_target(&move->receipt.transfer,&move->projected);
    if(status!=PXA_STATUS_OK)return status;
    if(move->projected.flags&PXA_INSTALL_RECORD_ARCHIVED)return run_archive(move,home);
    move->target->config.prepare_context=move;move->target->config.prepare_home=prepare_target;
    move->target->config.commit_context=move;move->target->config.commit_ready=handoff;
    move->target->config.media_context=move->target;move->target->config.media_ready=pxa_esp_installations_verify_media;
    if(move->receipt.transfer.phase>=PXA_INSTALL_TRANSFER_SURRENDERED) {
        status=pxa_posix_app_store_recover(&move->target->config,move->locator,result,home);
        if(status==PXA_STATUS_OK && same_record(move,&home->record,&move->projected))return finish_target(move,home);
        if(status!=PXA_STATUS_OK)return status;
        if(!move->source)return PXA_STATUS_UNAVAILABLE;
    }
    if(move->source) {
        status=source_location(move);if(status!=PXA_STATUS_OK)return status;
        if(move->source_retired && move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED) {
            /* Re-establish the rename barrier before issuing missing proof. */
            move->prepared=1;move->candidate.record=move->projected;status=handoff(move,&move->candidate);
            if(status!=PXA_STATUS_OK)return status;
        }
    }
    if(!move->source)return PXA_STATUS_UNAVAILABLE;
    int n=snprintf(move->source_package,sizeof(move->source_package),"%s/package",
        move->source_retired?move->retired:move->original.home);
    if(n<0 || (size_t)n>=sizeof(move->source_package))return PXA_STATUS_LIMIT_EXCEEDED;
    status=inspect_source(move,result);
    if(status!=PXA_STATUS_OK)return status;
    const pxa_package_manifest_t *manifest=*result->manifest;
    if(!manifest || manifest->encoded.size==0)return PXA_STATUS_DENIED;
    /* Full source payload and exact signed digest are checked again by the
     * target installer; the record alone never authorizes copied code. */
    if(!move->source_retired && !move->plan.reserved) {
        status=measure_source(move);
        if(status==PXA_STATUS_OK)status=reset_unpublished(move);
    }
    if(status==PXA_STATUS_OK)status=plan_target(move,manifest,1);
    if(status!=PXA_STATUS_OK)return status;
    move->target->plan=&move->plan;
    move->target->config.approved_manifest_sha256=move->receipt.transfer.source.manifest_sha256;
    status=seed_target(move);
    if(status==PXA_STATUS_OK)status=pxa_posix_app_store_install(&move->target->config,move->locator,
        move->source_package,move->receipt.transfer.source.generation,&move->projected,result,home);
    if(status==PXA_STATUS_OK)status=finish_target(move,home);
    return status;
}
static transfer_t *allocate(uint64_t target_epoch,const char *locator,const pxa_esp_app_transfer_control_t *control,
    pxa_status_t *status) {
    transfer_t *move=heap_caps_aligned_calloc(8,1,sizeof(*move),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move) {*status=PXA_STATUS_RESOURCE_LIMIT;return NULL;}
    move->source_parent=move->source_intent=move->target_directory=-1;move->control=control;move->stage="allocate";
    size_t length=locator?strnlen(locator,sizeof(move->locator)):0;
    if(!length || length>=sizeof(move->locator)) {*status=PXA_STATUS_INVALID_ARGUMENT;return move;}
    memcpy(move->locator,locator,length+1);
    *status=pxa_esp_installations_open(target_epoch,&move->target);if(*status!=PXA_STATUS_OK)return move;
    move->receipt_workspace=heap_caps_aligned_calloc(8,1,pxa_esp_transfer_receipt_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    move->copy_workspace=heap_caps_aligned_calloc(8,1,pxa_esp_private_copy_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move->receipt_workspace || !move->copy_workspace)*status=PXA_STATUS_RESOURCE_LIMIT;
    move->copy_config=(pxa_esp_private_copy_config_t){move->target->volume.allocation_unit,
        move->target->volume.max_file_bytes,0,0,move,copy_check};
    return move;
}
static pxa_status_t dispose(transfer_t *move,pxa_status_t status) {
    if(!move)return status;
    if(status!=PXA_STATUS_OK)ESP_LOGW("PxaTransfer","Move failed at %s: status=%ld phase=%u retired=%u",
        move->stage?move->stage:"unknown",(long)status,(unsigned)move->receipt.transfer.phase,(unsigned)move->source_retired);
    pxa_status_t closed=close_private(move);if(status==PXA_STATUS_OK)status=closed;
    /* Failed unmount retains the operation for retry rather than freeing its
     * backing image/borrowed plan. The Host must not start another move. */
    if(move->source_storage || move->target_storage) {pending_close=move;return status;}
    if(move->plan.reserved) {closed=pxa_esp_storage_release_plan(&move->plan);if(status==PXA_STATUS_OK)status=closed;}
    /* Refresh once at the cold mutation boundary. Cached desktop capacity
     * must not keep the pre-retirement nearly-full source measurement. */
    if(move->target_directory>=0 || move->source_intent>=0) {
        if(move->target)(void)pxa_esp_storage_refresh_volume(move->target->volume.mount_epoch);
        if(move->source)(void)pxa_esp_storage_refresh_volume(move->source->volume.mount_epoch);
    }
    if(move->target)move->target->plan=NULL;
    if(move->source_parent>=0) {close(move->source_parent);move->source_parent=-1;}
    if(move->source_intent>=0) {close(move->source_intent);move->source_intent=-1;}
    if(move->target_directory>=0) {close(move->target_directory);move->target_directory=-1;}
    closed=pxa_esp_installations_close(&move->source);if(status==PXA_STATUS_OK)status=closed;
    closed=pxa_esp_installations_close(&move->target);if(status==PXA_STATUS_OK)status=closed;
    if(move->source || move->target) {pending_close=move;return status;}
    if(pending_close==move)pending_close=NULL;
    heap_caps_free(move->owned_encoded);heap_caps_free(move->owned_manifest_workspace);
    heap_caps_free(move->receipt_workspace);heap_caps_free(move->copy_workspace);heap_caps_free(move);return status;
}
static pxa_status_t require_bound_manual_media(transfer_t *move) {
    if(move->original.record.flags&PXA_INSTALL_RECORD_HAS_MEDIA)return PXA_STATUS_OK;
    /* A pre-resource-package game can have an implicit app-volume media
     * root. Moving its record without first binding that root would silently
     * redirect assets to the destination. Keep that installation intact until
     * its media layout has been converted. */
    char publisher[PXA_POSIX_APP_LOCATOR_BYTES];
    pxa_status_t mapped=pxa_esp_publisher_map(move->source->root,&move->source->volume.id,
        "media",move->locator,0,publisher,sizeof(publisher));
    if(mapped!=PXA_STATUS_OK)return mapped;
    *strchr(publisher,'/')=0;
    const char *names[]={"media",publisher,move->original.record.app_id};
    int current=open(move->source->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(current<0)return io_error();
    pxa_status_t status=PXA_STATUS_UNSUPPORTED;
    for(unsigned i=0;i<3;++i) {
        int next=child(current,names[i],0);
        if(next<0) {status=errno==ENOENT?PXA_STATUS_OK:io_error();break;}
        if(close(current)!=0) {close(next);return io_error();}
        current=next;
    }
    if(close(current)!=0 && status==PXA_STATUS_OK)status=io_error();
    return status;
}
static pxa_status_t prepare_source(transfer_t *move,uint64_t source_epoch,uint64_t expected,
    const pxa_install_record_t *approved,pxa_posix_installer_result_t **result) {
    pxa_status_t status=pxa_esp_installations_open(source_epoch,&move->source);
    move->stage="lookup-source";
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_lookup(move->source,move->locator,&move->original);
    if(status==PXA_STATUS_OK && move->original.record.generation!=expected)status=PXA_STATUS_BAD_STATE;
    if(status==PXA_STATUS_OK && approved && !same_record(move,&move->original.record,approved))status=PXA_STATUS_BAD_STATE;
    if(status!=PXA_STATUS_OK)return status;
    char canonical[PXA_POSIX_APP_LOCATOR_BYTES];
    status=pxa_esp_installations_locator(move->original.record.publisher_root,move->original.record.app_id,canonical,sizeof(canonical));
    if(status!=PXA_STATUS_OK || strcmp(canonical,move->locator))return PXA_STATUS_DENIED;
    move->receipt.transfer.source=move->original.record;move->receipt.transfer.target_app_volume=move->target->volume.id;
    move->receipt.transfer.target_media_volume=move->original.record.media_volume;
    move->receipt.transfer.revision=1;move->receipt.transfer.phase=PXA_INSTALL_TRANSFER_COPYING;
    status=require_bound_manual_media(move);if(status!=PXA_STATUS_OK)return status;
    const int archived=!!(move->original.record.flags&PXA_INSTALL_RECORD_ARCHIVED);
    if(!archived) {status=owned_result(move,result);if(status!=PXA_STATUS_OK)return status;}
    status=measure_source(move);
    if(status!=PXA_STATUS_OK)return status;
    int parent=-1,directory=-1;
    status=home_directory(move->target,move->locator,0,&parent,&directory);
    if(directory>=0)close(directory);
    if(parent>=0)close(parent);
    if(status!=PXA_STATUS_NOT_FOUND)return status==PXA_STATUS_OK?PXA_STATUS_BAD_STATE:status;
    if(archived)return PXA_STATUS_OK;
    move->stage="inspect-source";
    status=pxa_posix_app_home_path(&move->original,PXA_POSIX_APP_HOME_PACKAGE,move->source_package,sizeof(move->source_package));
    if(status==PXA_STATUS_OK)status=inspect_source(move,*result);
    return status;
}
pxa_status_t pxa_esp_app_transfer_plan(uint64_t source_epoch,uint64_t target_epoch,const char *locator,
    uint64_t expected,pxa_install_record_t *record,pxa_install_plan_t *plan) {
    if(!record || !plan || !source_epoch || source_epoch==target_epoch || !expected || expected==UINT64_MAX)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(record,0,sizeof(*record));memset(plan,0,sizeof(*plan));
    if(pending_close)return PXA_STATUS_BUSY;
    pxa_status_t status;transfer_t *move=allocate(target_epoch,locator,NULL,&status);
    pxa_posix_installer_result_t *result=NULL;
    if(status==PXA_STATUS_OK)status=prepare_source(move,source_epoch,expected,NULL,&result);
    if(status==PXA_STATUS_OK)status=plan_target(move,result?*result->manifest:NULL,0);
    if(status==PXA_STATUS_OK) {*record=move->original.record;*plan=move->plan;}
    return dispose(move,status);
}
static pxa_status_t begin_move(uint64_t source_epoch,uint64_t target_epoch,const char *locator,
    uint64_t expected,const pxa_install_record_t *approved,const pxa_esp_app_transfer_control_t *control,
    pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    if(!home || !source_epoch || source_epoch==target_epoch || !expected || expected==UINT64_MAX)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(home,0,sizeof(*home));
    if(pending_close) {pxa_status_t retried=dispose(pending_close,PXA_STATUS_OK);if(pending_close)return retried;}
    pxa_status_t status;transfer_t *move=allocate(target_epoch,locator,control,&status);
    if(status==PXA_STATUS_OK)status=prepare_source(move,source_epoch,expected,approved,&result);
    if(status==PXA_STATUS_OK) {move->stage="admit";status=plan_target(move,
        (move->original.record.flags&PXA_INSTALL_RECORD_ARCHIVED)?NULL:*result->manifest,1);}
    if(status!=PXA_STATUS_OK)return dispose(move,status);
    esp_fill_random(move->receipt.transfer.transfer_id,16);nonce_text(move);
    int parent=-1;
    move->stage="create-target";status=home_directory(move->target,locator,1,&parent,&move->target_directory);
    if(parent>=0) {if(status==PXA_STATUS_OK && fsync(parent)!=0)status=io_error();close(parent);}
    if(status!=PXA_STATUS_OK)return dispose(move,status);
    move->target_receipt=(pxa_esp_transfer_receipt_config_t){move->target_directory,target_epoch,PXA_ESP_TRANSFER_TARGET,move->locator};
    move->stage="receipt-target";status=persist_phase(move,&move->target_receipt,PXA_INSTALL_TRANSFER_COPYING);
    if(status==PXA_STATUS_OK)status=source_intent(move,1);
    if(status==PXA_STATUS_OK) {move->stage="receipt-source";status=persist_phase(move,&move->source_receipt,PXA_INSTALL_TRANSFER_COPYING);}
    if(status==PXA_STATUS_OK) {
        ESP_LOGI("PxaTransfer","Transfer checkpoint ready: app=%s phase=copying",locator+65);
        if(control && control->progress)control->progress(control->context,1);
        status=run(move,result,home);
    }
    return dispose(move,status);
}
pxa_status_t pxa_esp_app_transfer_begin(uint64_t source_epoch,uint64_t target_epoch,const char *locator,
    uint64_t expected,const pxa_esp_app_transfer_control_t *control,pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    return begin_move(source_epoch,target_epoch,locator,expected,NULL,control,result,home);
}
pxa_status_t pxa_esp_app_transfer_begin_approved(uint64_t source_epoch,uint64_t target_epoch,const char *locator,
    const pxa_install_record_t *approved,const pxa_esp_app_transfer_control_t *control,
    pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    if(!approved)return PXA_STATUS_INVALID_ARGUMENT;
    return begin_move(source_epoch,target_epoch,locator,approved->generation,approved,control,result,home);
}
pxa_status_t pxa_esp_app_transfer_resume(uint64_t target_epoch,const char *locator,
    const pxa_esp_app_transfer_control_t *control,pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    if(!home)return PXA_STATUS_INVALID_ARGUMENT;
    memset(home,0,sizeof(*home));
    if(pending_close) {pxa_status_t retried=dispose(pending_close,PXA_STATUS_OK);if(pending_close)return retried;}
    pxa_status_t status;transfer_t *move=allocate(target_epoch,locator,control,&status);
    if(status!=PXA_STATUS_OK)return dispose(move,status);
    int parent=-1;
    status=home_directory(move->target,locator,0,&parent,&move->target_directory);if(parent>=0)close(parent);
    move->target_receipt=(pxa_esp_transfer_receipt_config_t){move->target_directory,target_epoch,PXA_ESP_TRANSFER_TARGET,move->locator};
    if(status==PXA_STATUS_OK)status=pxa_esp_transfer_receipt_load(&move->target_receipt,move->receipt_workspace,
        pxa_esp_transfer_receipt_workspace_size(),&move->receipt);
    if(status!=PXA_STATUS_OK)return dispose(move,status);
    if(move->receipt.transfer.kind!=PXA_INSTALL_TRANSFER_KIND_MOVE || move->receipt.context_size ||
        move->receipt.transfer.phase==PXA_INSTALL_TRANSFER_ABORTED)return dispose(move,PXA_STATUS_DENIED);
    nonce_text(move);pxa_volume_info_t source_volume;
    status=pxa_esp_storage_find_volume(&move->receipt.transfer.source.app_volume,&source_volume);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(source_volume.mount_epoch,&move->source);
    if(status==PXA_STATUS_OK) {
        move->original.record=move->receipt.transfer.source;strcpy(move->original.locator,move->locator);
        status=pxa_esp_publisher_path(move->source->volume.mount_epoch,"apps",move->locator,move->original.home,sizeof(move->original.home));
        if(status!=PXA_STATUS_OK)return dispose(move,status);
        status=source_intent(move,move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED);
        if(status==PXA_STATUS_OK)status=persist_phase(move,&move->source_receipt,
            move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED?PXA_INSTALL_TRANSFER_COPYING:move->receipt.transfer.phase);
        if(status!=PXA_STATUS_OK)return dispose(move,status);
    } else if(move->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED)return dispose(move,PXA_STATUS_UNAVAILABLE);
    status=(move->receipt.transfer.source.flags&PXA_INSTALL_RECORD_ARCHIVED)?PXA_STATUS_OK:owned_result(move,&result);
    if(status==PXA_STATUS_OK)status=run(move,result,home);
    return dispose(move,status);
}
typedef struct {
    pxa_volume_info_t volume;
    uint8_t publisher[32];
    char mount[PXA_ESP_STORAGE_PATH_BYTES],locator[PXA_POSIX_APP_LOCATOR_BYTES];
} transfer_scan_t;
static int publisher_bytes(const char *name,uint8_t bytes[32]) {
    if(strlen(name)!=64)return 0;
    for(unsigned i=0;i<32;++i) {
        unsigned value=0;
        for(unsigned j=0;j<2;++j) {
            unsigned char c=(unsigned char)name[2*i+j];
            if(c>='0' && c<='9')value=value*16+(c-'0');
            else if(c>='a' && c<='f')value=value*16+(c-'a'+10);
            else return 0;
        }
        bytes[i]=(uint8_t)value;
    }
    return 1;
}
static DIR *directory_stream(int directory) {
    int copy=dup(directory);if(copy<0)return NULL;
    DIR *stream=fdopendir(copy);if(!stream) {int saved=errno;close(copy);errno=saved;}
    return stream;
}
pxa_status_t pxa_esp_app_transfer_visit(uint64_t epoch,pxa_esp_app_transfer_visitor_fn visitor,void *context,size_t *count) {
    if(!visitor || !count)return PXA_STATUS_INVALID_ARGUMENT;
    *count=0;pxa_status_t status=pxa_esp_volumes_retain(epoch);if(status!=PXA_STATUS_OK)return status;
    transfer_scan_t *scan=heap_caps_aligned_calloc(8,1,sizeof(*scan),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!scan) {pxa_esp_volumes_release(epoch);return PXA_STATUS_RESOURCE_LIMIT;}
    int root=-1,pxa=-1,apps=-1,publisher=-1;DIR *publishers=NULL,*applications=NULL;
    status=pxa_esp_storage_volume(epoch,&scan->volume);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume_path(epoch,scan->mount,sizeof(scan->mount));
    if(status!=PXA_STATUS_OK || scan->volume.state!=PXA_VOLUME_READY || scan->volume.ambiguous) {status=PXA_STATUS_UNAVAILABLE;goto done;}
    root=open(scan->mount,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root>=0)pxa=child(root,"PXA",0);
    if(pxa>=0)apps=child(pxa,"apps",0);
    if(apps<0) {status=errno==ENOENT?PXA_STATUS_OK:io_error();goto done;}
    close(root);root=-1;close(pxa);pxa=-1;
    publishers=directory_stream(apps);if(!publishers) {status=io_error();goto done;}
    for(;;) {
        errno=0;struct dirent *group=readdir(publishers);
        if(!group) {if(errno)status=io_error();break;}
        char physical[PXA_POSIX_APP_LOCATOR_BYTES],logical[PXA_POSIX_APP_LOCATOR_BYTES],pxa_root[PXA_ESP_STORAGE_PATH_BYTES+8];
        if(snprintf(physical,sizeof(physical),"%s/probe",group->d_name)>=(int)sizeof(physical))continue;
        if(snprintf(pxa_root,sizeof(pxa_root),"%s/PXA",scan->mount)>=(int)sizeof(pxa_root))continue;
        if(pxa_esp_publisher_map(pxa_root,&scan->volume.id,"apps",physical,1,logical,sizeof(logical))!=PXA_STATUS_OK)continue;
        *strchr(logical,'/')=0;
        if(!publisher_bytes(logical,scan->publisher))continue;
        publisher=child(apps,group->d_name,0);
        if(publisher<0) {if(errno==ENOTDIR || errno==ELOOP || errno==ENOENT)continue;status=io_error();break;}
        applications=directory_stream(publisher);if(!applications) {status=io_error();break;}
        int keep_going=1;
        for(;;) {
            errno=0;struct dirent *app=readdir(applications);
            if(!app) {if(errno)status=io_error();break;}
            if(pxa_esp_installations_locator(scan->publisher,app->d_name,scan->locator,sizeof(scan->locator))!=PXA_STATUS_OK)continue;
            int home=child(publisher,app->d_name,0);
            if(home<0) {if(errno==ENOTDIR || errno==ELOOP || errno==ENOENT)continue;status=io_error();break;}
            uint8_t pending=0;const char *names[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1,
                PXA_ESP_TRANSFER_SLOT_0 ".pending",PXA_ESP_TRANSFER_SLOT_1 ".pending"};
            for(unsigned i=0;i<4;++i) {
                struct stat metadata;
                if(fstatat(home,names[i],&metadata,AT_SYMLINK_NOFOLLOW)==0) {pending=1;break;}
                if(errno!=ENOENT) {status=io_error();break;}
            }
            if(close(home)!=0 && status==PXA_STATUS_OK)status=io_error();
            if(status!=PXA_STATUS_OK)break;
            if(pending) {++*count;keep_going=visitor(context,scan->locator);if(!keep_going)break;}
            status=pxa_esp_storage_volume(epoch,&scan->volume);
            if(status!=PXA_STATUS_OK || scan->volume.state!=PXA_VOLUME_READY || scan->volume.ambiguous) {status=PXA_STATUS_UNAVAILABLE;break;}
        }
        if(closedir(applications)!=0 && status==PXA_STATUS_OK)status=io_error();
        applications=NULL;
        if(close(publisher)!=0 && status==PXA_STATUS_OK)status=io_error();
        publisher=-1;
        if(status!=PXA_STATUS_OK || !keep_going)break;
    }
done:
    if(applications && closedir(applications)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(publisher>=0 && close(publisher)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(publishers && closedir(publishers)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(apps>=0 && close(apps)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(pxa>=0 && close(pxa)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(root>=0 && close(root)!=0 && status==PXA_STATUS_OK)status=io_error();
    heap_caps_free(scan);pxa_esp_volumes_release(epoch);return status;
}
