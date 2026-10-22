#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_resource_transfer.h"
#include "pxa_esp_installations_internal.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_volumes.h"
#include "pxa/openssl/pxa_openssl.h"
#include "esp_heap_caps.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define close pxa_esp_close
#define fstat pxa_esp_fstat
#define fstatat pxa_esp_fstatat
#endif

typedef struct {
    pxa_esp_installations_t *app,*source,*target;
    pxa_posix_app_home_t home;
    pxa_install_record_t projected;
    pxa_install_plan_t plan,resource_plan;
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *manifest;
    void *workspace;
    uint8_t *encoded;
    char source_package[512],result_root[512],relative[256];
    struct stat before,after;
    uint8_t reuse;
} resource_move_t;

static int equal_record(const pxa_install_record_t *a,const pxa_install_record_t *b) {
    uint8_t first[PXA_INSTALL_RECORD_BYTES],second[PXA_INSTALL_RECORD_BYTES];
    return !pxa_install_record_encode(a,first) && !pxa_install_record_encode(b,second) &&
        !memcmp(first,second,sizeof(first));
}
static pxa_status_t dispose(resource_move_t *move,pxa_status_t status) {
    if(!move)return status;
    if(move->plan.reserved) {
        /* Refresh while precise mounts are still retained; later admission
         * must see the actual copied generation and local record footprint. */
        (void)pxa_esp_storage_refresh_volume(move->app->volume.mount_epoch);
        if(move->target->volume.mount_epoch!=move->app->volume.mount_epoch)
            (void)pxa_esp_storage_refresh_volume(move->target->volume.mount_epoch);
        pxa_status_t released=pxa_esp_storage_release_plan(&move->plan);
        if(status==PXA_STATUS_OK)status=released;
    }
    pxa_status_t closed=pxa_esp_installations_close(&move->app);
    if(status==PXA_STATUS_OK)status=closed;
    closed=pxa_esp_installations_close(&move->source);if(status==PXA_STATUS_OK)status=closed;
    closed=pxa_esp_installations_close(&move->target);if(status==PXA_STATUS_OK)status=closed;
    /* These owners never mount private filesystems: close cannot retain an
     * unmount failure, and no new resident task/stack/buffer is needed. */
    heap_caps_free(move->encoded);heap_caps_free(move->workspace);heap_caps_free(move);
    return status;
}
static pxa_status_t read_manifest(resource_move_t *move) {
    int n=snprintf(move->result_root,sizeof(move->result_root),"%s/manifest.pxm",move->source_package);
    if(n<0 || (size_t)n>=sizeof(move->result_root))return PXA_STATUS_LIMIT_EXCEEDED;
    int file=open(move->result_root,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(file<0)return PXA_STATUS_DENIED;
    pxa_status_t status=PXA_STATUS_OK;
    if(fstat(file,&move->before)!=0 || !S_ISREG(move->before.st_mode) || move->before.st_nlink>1 ||
        move->before.st_size<=0)status=PXA_STATUS_DENIED;
    else if((uint64_t)move->before.st_size>PXA_ESP_SIGNED_MANIFEST_MAX_BYTES)status=PXA_STATUS_LIMIT_EXCEEDED;
    size_t size=status==PXA_STATUS_OK?(size_t)move->before.st_size:0,offset=0;
    if(status==PXA_STATUS_OK) {
        move->encoded=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!move->encoded)status=PXA_STATUS_RESOURCE_LIMIT;
    }
    while(status==PXA_STATUS_OK && offset<size) {
        ssize_t count=read(file,move->encoded+offset,size-offset);
        if(count<0 && errno==EINTR)continue;
        if(count<=0) {status=PXA_STATUS_IO_ERROR;break;}
        offset+=(size_t)count;
    }
    if(status==PXA_STATUS_OK && (fstat(file,&move->after)!=0 ||
        move->before.st_size!=move->after.st_size || move->before.st_dev!=move->after.st_dev ||
        move->before.st_ino!=move->after.st_ino || move->before.st_mtime!=move->after.st_mtime))status=PXA_STATUS_DENIED;
    if(close(file)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    if(status!=PXA_STATUS_OK)return status;
    pxa_package_limits_t limits;
    status=pxa_package_manifest_measure((pxa_bytes_t){move->encoded,size},&move->source->config.installer.limits,&limits);
    if(status!=PXA_STATUS_OK)return status;
    size_t bytes=pxa_package_manifest_workspace_size(&limits);
    if(!bytes)return PXA_STATUS_DENIED;
    move->workspace=heap_caps_aligned_calloc(8,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move->workspace)return PXA_STATUS_RESOURCE_LIMIT;
    move->result=(pxa_posix_installer_result_t){.struct_size=sizeof(move->result),
        .manifest=&move->manifest,.manifest_workspace=move->workspace,.manifest_workspace_size=bytes,
        .encoded=move->encoded,.encoded_capacity=size,.root=move->result_root,.root_capacity=sizeof(move->result_root)};
    status=pxa_esp_installations_inspect(move->source,move->source_package,&move->result);
    if(status!=PXA_STATUS_OK)return status;
    const pxa_install_record_t *record=&move->home.record;
    uint8_t digest[32];status=pxa_openssl_sha256(move->manifest->encoded.data,move->manifest->encoded.size,digest);
    if(status==PXA_STATUS_OK && (move->manifest->package_kind!=PXA_PACKAGE_KIND_RESOURCES ||
        memcmp(digest,record->media_set_sha256,32) || memcmp(move->manifest->management_key_id,record->publisher_root,32) ||
        move->manifest->app_id.size!=strlen(record->app_id) ||
        memcmp(move->manifest->app_id.data,record->app_id,move->manifest->app_id.size)))status=PXA_STATUS_DENIED;
    return status;
}
static pxa_status_t prepare(resource_move_t *move,uint64_t app_epoch,uint64_t target_epoch,
    const char *locator,uint64_t generation,const pxa_install_record_t *approved,uint64_t media_epoch) {
    pxa_status_t status=pxa_esp_installations_open(app_epoch,&move->app);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_lookup(move->app,locator,&move->home);
    if(status!=PXA_STATUS_OK)return status;
    const pxa_install_record_t *record=&move->home.record;
    const uint32_t managed=PXA_INSTALL_RECORD_HAS_MEDIA|PXA_INSTALL_RECORD_MANAGED_MEDIA;
    if(record->generation!=generation || (approved && !equal_record(record,approved)))return PXA_STATUS_BAD_STATE;
    if((record->flags&managed)!=managed || (record->flags&PXA_INSTALL_RECORD_ARCHIVED))return PXA_STATUS_UNSUPPORTED;
    char canonical[PXA_POSIX_APP_LOCATOR_BYTES];
    status=pxa_esp_installations_locator(record->publisher_root,record->app_id,canonical,sizeof(canonical));
    if(status!=PXA_STATUS_OK || strcmp(canonical,locator))return PXA_STATUS_DENIED;
    pxa_volume_info_t volume;
    status=pxa_esp_storage_find_volume(&record->media_volume,&volume);
    if(status!=PXA_STATUS_OK)return status;
    if(media_epoch && media_epoch!=volume.mount_epoch)return PXA_STATUS_BAD_STATE;
    if(volume.mount_epoch==target_epoch)return PXA_STATUS_INVALID_ARGUMENT;
    status=pxa_esp_installations_open(volume.mount_epoch,&move->source);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(target_epoch,&move->target);
    if(status!=PXA_STATUS_OK)return status;
    move->projected=*record;move->projected.media_volume=move->target->volume.id;
    status=pxa_esp_installations_media_path(record,move->relative,sizeof(move->relative));
    if(status!=PXA_STATUS_OK)return status;
    int n=snprintf(move->source_package,sizeof(move->source_package),"%s/%s",move->source->root,move->relative+4);
    if(n<0 || (size_t)n>=sizeof(move->source_package))return PXA_STATUS_LIMIT_EXCEEDED;
    status=read_manifest(move);if(status!=PXA_STATUS_OK)return status;
    /* Preview probes presence only. Commit fully verifies reuse; a damaged
     * generation is never silently repaired or overwritten by relocation. */
    n=snprintf(move->result_root,sizeof(move->result_root),"%s/%s",move->target->root,move->relative+4);
    if(n<0 || (size_t)n>=sizeof(move->result_root))return PXA_STATUS_LIMIT_EXCEEDED;
    struct stat *info=&move->after;
    if(fstatat(AT_FDCWD,move->result_root,info,AT_SYMLINK_NOFOLLOW)==0) {
        if(!S_ISDIR(info->st_mode))return PXA_STATUS_DENIED;
        move->reuse=1;
    } else if(errno!=ENOENT)return PXA_STATUS_IO_ERROR;
    status=pxa_esp_storage_refresh_volume(app_epoch);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_refresh_volume(target_epoch);
    if(status!=PXA_STATUS_OK)return status;
    uint64_t records=0;
    status=pxa_install_file_allocation(&move->app->volume,PXA_POSIX_INSTALL_RECORD_SEALED_BYTES,&records);
    if(status!=PXA_STATUS_OK)return status;
    uint64_t media_bytes=0;
    if(!move->reuse) {
        status=pxa_esp_installations_plan(move->target,move->manifest,0,0,&move->resource_plan);
        if(status!=PXA_STATUS_OK)return status;
        media_bytes=move->resource_plan.volumes[0].bytes;
    }
    /* No second code package or private image is staged on the app volume.
     * Resources stream directory-to-directory on the chosen resource volume. */
    pxa_install_requirements_t requirements={3*records+4096,0,media_bytes,0,0};
    return pxa_esp_storage_plan(&requirements,app_epoch,target_epoch,&move->plan);
}
static pxa_status_t verified_destination(void *context,const pxa_install_record_t *record) {
    resource_move_t *move=context;pxa_volume_info_t volume;
    pxa_status_t status=pxa_esp_storage_volume(move->target->volume.mount_epoch,&volume);
    if(status!=PXA_STATUS_OK || volume.state!=PXA_VOLUME_READY || volume.ambiguous)return PXA_STATUS_UNAVAILABLE;
    return move->target->resource_verified && equal_record(record,&move->projected)?PXA_STATUS_OK:PXA_STATUS_DENIED;
}
pxa_status_t pxa_esp_resource_transfer_plan(uint64_t app_epoch,uint64_t target_epoch,const char *locator,
    uint64_t generation,pxa_install_record_t *record,uint64_t *media_epoch,pxa_install_plan_t *plan) {
    if(!app_epoch || !target_epoch || !locator || !record || !media_epoch || !plan ||
        !generation || generation==UINT64_MAX)return PXA_STATUS_INVALID_ARGUMENT;
    memset(record,0,sizeof(*record));memset(plan,0,sizeof(*plan));*media_epoch=0;
    resource_move_t *move=heap_caps_calloc(1,sizeof(*move),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_status_t status=prepare(move,app_epoch,target_epoch,locator,generation,NULL,0);
    if(status==PXA_STATUS_OK) {*record=move->home.record;*media_epoch=move->source->volume.mount_epoch;*plan=move->plan;}
    return dispose(move,status);
}
pxa_status_t pxa_esp_resource_transfer_begin_approved(uint64_t app_epoch,uint64_t media_epoch,
    uint64_t target_epoch,const char *locator,const pxa_install_record_t *approved,pxa_posix_app_home_t *home) {
    if(home)memset(home,0,sizeof(*home));
    if(!app_epoch || !media_epoch || !target_epoch || !locator || !approved || !home ||
        !approved->generation || approved->generation==UINT64_MAX)return PXA_STATUS_INVALID_ARGUMENT;
    resource_move_t *move=heap_caps_calloc(1,sizeof(*move),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!move)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_status_t status=prepare(move,app_epoch,target_epoch,locator,approved->generation,approved,media_epoch);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_reserve_plan(&move->plan);
    if(status==PXA_STATUS_OK) {
        if(move->reuse)status=pxa_esp_installations_verify_resources(move->target,&move->projected,&move->result);
        else status=pxa_esp_installations_install_resources(move->target,&move->projected,move->source_package,&move->plan,&move->result);
    }
    if(status==PXA_STATUS_OK) {
        move->app->config.media_ready=verified_destination;move->app->config.media_context=move;
        status=pxa_posix_app_store_rebind_media(&move->app->config,locator,approved,&move->projected.media_volume,home);
        move->app->config.media_ready=NULL;move->app->config.media_context=NULL;
    }
    return dispose(move,status);
}
