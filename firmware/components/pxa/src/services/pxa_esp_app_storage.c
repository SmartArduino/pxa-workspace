#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_app_storage.h"
#include "pxa_esp_image_blockdev.h"
#include "pxa_esp_volumes.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_publisher_directory.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(CONFIG_LITTLEFS_SPIFFS_COMPAT) && CONFIG_LITTLEFS_SPIFFS_COMPAT
#error "PXA private storage requires normal LittleFS directories: disable CONFIG_LITTLEFS_SPIFFS_COMPAT"
#endif

struct pxa_esp_app_storage {
    uint64_t epoch,capacity,reserved_bytes,data_generation,media_epoch;
    const pxa_install_plan_t *plan;
    esp_blockdev_handle_t image;
    uint8_t native_private,mounted,alias_bound,home_bound,ready;
    uint8_t authority_binding[32],install_id[16];
    char identity[130];
    char home[PXA_POSIX_APP_PATH_BYTES],root[PXA_POSIX_APP_PATH_BYTES],alias[16];
};
static pthread_mutex_t g_alias_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_alias_sequence;
uint64_t pxa_esp_app_storage_media_epoch(const pxa_esp_app_storage_t *storage) {
    return storage?storage->media_epoch:0;
}
pxa_status_t pxa_esp_app_storage_bind_media(pxa_esp_app_storage_t *storage,uint64_t epoch) {
    if(!storage || !epoch)return PXA_STATUS_INVALID_ARGUMENT;
    if(storage->media_epoch)return storage->media_epoch==epoch?PXA_STATUS_OK:PXA_STATUS_BAD_STATE;
    pxa_status_t status=pxa_esp_volumes_retain(epoch);
    if(status==PXA_STATUS_OK)storage->media_epoch=epoch;
    return status;
}
static const char image_name[]="private.lfs",pending_name[]="private.pending.lfs";
static const uint8_t permission_purpose[]="pxa.permission.allow.v1";
static pxa_status_t authority_identity(pxa_esp_app_storage_t *storage,const pxa_install_record_t *record) {
    static const char hex[]="0123456789abcdef";
    uint8_t binding[16+16+32+2+64+8];size_t app_bytes=strlen(record->app_id),binding_bytes=66+app_bytes;
    memcpy(binding,record->app_volume.bytes,16);memcpy(binding+16,record->install_id,16);
    memcpy(binding+32,record->publisher_root,32);pxa_write_u16(binding+64,(uint16_t)app_bytes);
    memcpy(binding+66,record->app_id,app_bytes);
    if(record->data_generation) {
        pxa_write_u64(binding+binding_bytes,record->data_generation);binding_bytes+=8;
    }
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!md || mbedtls_md(md,binding,binding_bytes,storage->authority_binding))return PXA_STATUS_INTERNAL;
    for(unsigned i=0;i<32;++i) {
        storage->identity[2*i]=hex[record->publisher_root[i]>>4];
        storage->identity[2*i+1]=hex[record->publisher_root[i]&15];
    }
    storage->identity[64]=':';memcpy(storage->identity+65,record->app_id,app_bytes+1);
    return PXA_STATUS_OK;
}

static pxa_status_t io_error(void) {
    if(errno==ENOSPC)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==EMFILE || errno==ENFILE || errno==ENOMEM)return PXA_STATUS_RESOURCE_LIMIT;
    return PXA_STATUS_IO_ERROR;
}
static pxa_status_t ready(pxa_esp_app_storage_t *storage) {
    pxa_volume_info_t info;
    pxa_status_t status=pxa_esp_storage_volume(storage->epoch,&info);
    if(status!=PXA_STATUS_OK || info.state!=PXA_VOLUME_READY || info.ambiguous)return PXA_STATUS_UNAVAILABLE;
    return (info.capabilities&(PXA_VOLUME_READ|PXA_VOLUME_WRITE))==(PXA_VOLUME_READ|PXA_VOLUME_WRITE)?
        PXA_STATUS_OK:PXA_STATUS_DENIED;
}
pxa_status_t pxa_esp_app_storage_create(const pxa_posix_app_home_t *home,uint64_t epoch,
    uint64_t capacity,const pxa_install_plan_t *plan,pxa_esp_app_storage_t **output) {
    pxa_esp_app_storage_t *storage;pxa_volume_info_t info;pxa_status_t status;
    char expected_home[PXA_POSIX_APP_PATH_BYTES];
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;
    if(!home || !epoch || !memchr(home->record.app_id,0,sizeof(home->record.app_id)) ||
        !memchr(home->locator,0,sizeof(home->locator)) ||
        !memchr(home->home,0,sizeof(home->home)))return PXA_STATUS_INVALID_ARGUMENT;
    const char *slash=strchr(home->locator,'/');
    if(!slash || slash==home->locator || slash-home->locator>64 || strchr(slash+1,'/') ||
        strcmp(slash+1,home->record.app_id) || !pxa_fs_scoped_path_is_valid(
            (pxa_bytes_t){(const uint8_t *)home->locator,strlen(home->locator)}))return PXA_STATUS_DENIED;
    static const char hex[]="0123456789abcdef";
    if(slash-home->locator!=64)return PXA_STATUS_DENIED;
    for(unsigned i=0;i<32;++i)
        if(home->locator[2*i]!=hex[home->record.publisher_root[i]>>4] ||
            home->locator[2*i+1]!=hex[home->record.publisher_root[i]&15])return PXA_STATUS_DENIED;
    /* Maintenance also serves disabled/archived applications. The activation
     * owner checks execution policy before exposing a session to services. */
    status=pxa_esp_storage_volume(epoch,&info);if(status!=PXA_STATUS_OK)return status;
    if(info.state!=PXA_VOLUME_READY || info.ambiguous || memcmp(info.id.bytes,home->record.app_volume.bytes,16))
        return PXA_STATUS_UNAVAILABLE;
    if(!(info.capabilities&PXA_VOLUME_PRIVATE_NATIVE) &&
        (capacity<65536 || capacity>INT32_MAX || capacity%4096))return PXA_STATUS_INVALID_ARGUMENT;
    status=pxa_esp_publisher_path(epoch,"apps",home->locator,expected_home,sizeof(expected_home));if(status!=PXA_STATUS_OK)return status;
    storage=heap_caps_calloc(1,sizeof(*storage),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!storage)storage=heap_caps_calloc(1,sizeof(*storage),MALLOC_CAP_8BIT);
    if(!storage)return PXA_STATUS_RESOURCE_LIMIT;
    if(strlen(expected_home)>=sizeof(storage->home) || strcmp(expected_home,home->home)) {
        heap_caps_free(storage);return PXA_STATUS_DENIED;
    }
    strcpy(storage->home,expected_home);
    status=pxa_posix_app_home_path(home,PXA_POSIX_APP_HOME_PRIVATE,storage->root,sizeof(storage->root));
    if(status!=PXA_STATUS_OK) {heap_caps_free(storage);return status;}
    storage->epoch=epoch;storage->capacity=capacity;storage->plan=plan;
    storage->native_private=!!(info.capabilities&PXA_VOLUME_PRIVATE_NATIVE);
    storage->data_generation=home->record.data_generation;
    memcpy(storage->install_id,home->record.install_id,sizeof(storage->install_id));
    status=authority_identity(storage,&home->record);
    if(status==PXA_STATUS_OK)status=ready(storage);
    if(status==PXA_STATUS_OK)status=pxa_esp_volumes_bind_app_home(storage->home,epoch);
    if(status!=PXA_STATUS_OK) {heap_caps_free(storage);return status;}
    storage->home_bound=1;*output=storage;return PXA_STATUS_OK;
}

static pxa_status_t reserve_image(pxa_esp_app_storage_t *storage) {
    pxa_status_t status;pxa_volume_info_t info;uint64_t bytes=0;
    pxa_esp_volumes_lock();
    if(storage->plan) {
        status=storage->plan->app_mount_epoch==storage->epoch?
            pxa_install_plan_validate(pxa_esp_volumes_registry(),storage->plan):PXA_STATUS_DENIED;
        if(status==PXA_STATUS_OK) {
            status=pxa_esp_storage_volume(storage->epoch,&info);
            if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&info,storage->capacity,&bytes);
            if(status==PXA_STATUS_OK) {
                uint64_t headroom=(uint64_t)info.allocation_unit*3+4096;
                if(bytes>UINT64_MAX-headroom)status=PXA_STATUS_LIMIT_EXCEEDED;
                else {
                    bytes+=headroom;status=PXA_STATUS_QUOTA_EXCEEDED;
                    for(unsigned i=0;i<storage->plan->volume_count;++i)
                        if(storage->plan->volumes[i].mount_epoch==storage->epoch &&
                            storage->plan->volumes[i].bytes>=bytes)status=PXA_STATUS_OK;
                }
            }
        }
        pxa_esp_volumes_unlock();return status;
    }
    status=pxa_esp_storage_refresh_volume(storage->epoch);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume(storage->epoch,&info);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&info,storage->capacity,&bytes);
    /* Cover the private directory, inactive FAT/LittleFS metadata and an I/O
     * failure's cleanup. This is physical admission, not a logical save quota. */
    if(status==PXA_STATUS_OK) {
        uint64_t headroom=(uint64_t)info.allocation_unit*3+4096;
        if(bytes>UINT64_MAX-headroom)status=PXA_STATUS_LIMIT_EXCEEDED;
        else {
            bytes+=headroom;
            status=pxa_volume_reserve(pxa_esp_volumes_registry(),storage->epoch,bytes);
            if(status==PXA_STATUS_OK)storage->reserved_bytes=bytes;
        }
    }
    pxa_esp_volumes_unlock();return status;
}
static void release_image(pxa_esp_app_storage_t *storage) {
    if(!storage->reserved_bytes)return;
    pxa_esp_volumes_lock();
    (void)pxa_volume_release(pxa_esp_volumes_registry(),storage->epoch,storage->reserved_bytes);
    storage->reserved_bytes=0;(void)pxa_esp_storage_refresh_volume(storage->epoch);
    pxa_esp_volumes_unlock();
}

static pxa_status_t open_or_prepare_image(pxa_esp_app_storage_t *storage,int directory) {
    int file=pxa_esp_openat(directory,image_name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    pxa_status_t status;
    if(file>=0) {
        status=pxa_esp_image_blockdev_take(file,storage->epoch,&storage->image);
        if(status!=PXA_STATUS_OK)pxa_esp_close(file);
        return status;
    }
    if(errno!=ENOENT)return io_error();
    /* Do not hide retained legacy data behind a newly formatted image. */
    int scan=pxa_esp_dup(directory);
    if(scan<0)return io_error();
    DIR *stream=pxa_esp_fdopendir(scan);struct dirent *entry;
    if(!stream) {pxa_esp_close(scan);return io_error();}
    errno=0;
    while((entry=readdir(stream))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..") || !strcmp(entry->d_name,pending_name))continue;
        closedir(stream);return PXA_STATUS_BAD_STATE;
    }
    int failure=errno;
    if(closedir(stream)!=0 && !failure)failure=errno;
    if(failure) {errno=failure;return io_error();}
    status=reserve_image(storage);if(status!=PXA_STATUS_OK)return status;
    /* A previous uncommitted initialization is disposable. The final image
     * is never truncated, reformatted or replaced by this path. */
    if(pxa_esp_unlinkat(directory,pending_name,0)!=0 && errno!=ENOENT)return io_error();
    file=pxa_esp_openat(directory,pending_name,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(file<0)return io_error();
    status=pxa_esp_image_blockdev_prepare(file,storage->epoch,storage->capacity);
    if(status==PXA_STATUS_OK)status=pxa_esp_image_blockdev_take(file,storage->epoch,&storage->image);
    if(!storage->image)pxa_esp_close(file);
    if(status==PXA_STATUS_OK && esp_littlefs_format_blockdev(storage->image)!=ESP_OK)status=PXA_STATUS_IO_ERROR;
    if(storage->image) {
        pxa_status_t closed=pxa_esp_image_blockdev_close(&storage->image);
        if(status==PXA_STATUS_OK)status=closed;
    }
    if(status!=PXA_STATUS_OK) {pxa_esp_unlinkat(directory,pending_name,0);return status;}
    /* FAT keeps directory-entry state in an open FIL: close before rename. */
    struct stat metadata;
    if(pxa_esp_fstatat(directory,image_name,&metadata,AT_SYMLINK_NOFOLLOW)==0)return PXA_STATUS_BUSY;
    if(errno!=ENOENT)return io_error();
    if(pxa_esp_renameat(directory,pending_name,directory,image_name)!=0)return io_error();
    if(pxa_esp_fsync(directory)!=0)return io_error();
    file=pxa_esp_openat(directory,image_name,O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(file<0)return io_error();
    status=pxa_esp_image_blockdev_take(file,storage->epoch,&storage->image);
    if(status!=PXA_STATUS_OK)pxa_esp_close(file);
    return status;
}

pxa_status_t pxa_esp_app_storage_existing_root(pxa_esp_app_storage_t *storage,const char **output) {
    if(!storage || !output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;pxa_status_t status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    if(storage->ready) {*output=storage->root;return PXA_STATUS_OK;}
    int directory=pxa_esp_open(storage->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)return errno==ENOENT?PXA_STATUS_NOT_FOUND:io_error();
    if(!storage->native_private) {
        struct stat metadata;
        if(pxa_esp_fstatat(directory,image_name,&metadata,AT_SYMLINK_NOFOLLOW)!=0)
            status=errno==ENOENT?PXA_STATUS_NOT_FOUND:io_error();
        else if(!S_ISREG(metadata.st_mode) || metadata.st_nlink>1)status=PXA_STATUS_DENIED;
    }
    if(pxa_esp_close(directory)!=0 && status==PXA_STATUS_OK)status=io_error();
    return status==PXA_STATUS_OK?pxa_esp_app_storage_root(storage,output):status;
}

pxa_status_t pxa_esp_app_storage_root(pxa_esp_app_storage_t *storage,const char **output) {
    pxa_status_t status;int directory;
    if(!storage || !output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    if(storage->ready) {*output=storage->root;return PXA_STATUS_OK;}
    if(storage->data_generation) {
        char *separator=strrchr(storage->root,'/');
        *separator=0;int created=mkdir(storage->root,0700),error=errno;*separator='/';
        if(created!=0 && error!=EEXIST) {errno=error;return io_error();}
    }
    if(mkdir(storage->root,0700)!=0 && errno!=EEXIST)return io_error();
    directory=pxa_esp_open(storage->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)return io_error();
    if(storage->native_private) {
        pxa_esp_close(directory);storage->ready=1;*output=storage->root;return PXA_STATUS_OK;
    }
    status=open_or_prepare_image(storage,directory);pxa_esp_close(directory);release_image(storage);
    if(status!=PXA_STATUS_OK)return status;
    pthread_mutex_lock(&g_alias_lock);
    uint32_t sequence=g_alias_sequence==UINT32_MAX?0:++g_alias_sequence;
    pthread_mutex_unlock(&g_alias_lock);
    if(!sequence) {pxa_esp_image_blockdev_close(&storage->image);return PXA_STATUS_RESOURCE_LIMIT;}
    snprintf(storage->alias,sizeof(storage->alias),"/_pxa/i%08x",(unsigned)sequence);
    status=pxa_esp_volumes_bind_private(storage->alias,storage->epoch);
    if(status!=PXA_STATUS_OK) {pxa_esp_image_blockdev_close(&storage->image);return status;}
    storage->alias_bound=1;
    const esp_vfs_littlefs_conf_t config={.base_path=storage->alias,.blockdev=storage->image,
        .format_if_mount_failed=false,.read_only=false,.dont_mount=false,.grow_on_mount=false};
    esp_err_t mounted=esp_vfs_littlefs_register(&config);
    if(mounted!=ESP_OK) {
        ESP_LOGW("PxaPrivate","Private mount failed: %s",esp_err_to_name(mounted));
        pxa_esp_volumes_unbind_private(storage->alias,storage->epoch);storage->alias_bound=0;
        pxa_esp_image_blockdev_close(&storage->image);
        return mounted==ESP_ERR_NO_MEM?PXA_STATUS_RESOURCE_LIMIT:PXA_STATUS_IO_ERROR;
    }
    storage->mounted=1;storage->ready=1;strcpy(storage->root,storage->alias);
    *output=storage->root;return PXA_STATUS_OK;
}

static pxa_status_t permission_key(pxa_esp_app_storage_t *storage,pxa_bytes_t identity,
    pxa_bytes_t name,pxa_bytes_t scope,uint8_t digest[32],char filename[65]) {
    static const char hex[]="0123456789abcdef";uint8_t sizes[16];mbedtls_md_context_t hash;
    if(!identity.data || identity.size!=strlen(storage->identity) ||
        memcmp(identity.data,storage->identity,identity.size))return PXA_STATUS_DENIED;
    if(!name.data || !name.size || name.size>96 || scope.size>1024 || (!scope.data && scope.size))
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    pxa_write_u64(sizes,name.size);pxa_write_u64(sizes+8,scope.size);
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_init(&hash);
    int failed=!md || mbedtls_md_setup(&hash,md,0) || mbedtls_md_starts(&hash) ||
        mbedtls_md_update(&hash,sizes,sizeof(sizes)) || mbedtls_md_update(&hash,name.data,name.size) ||
        (scope.size && mbedtls_md_update(&hash,scope.data,scope.size)) || mbedtls_md_finish(&hash,digest);
    mbedtls_md_free(&hash);if(failed)return PXA_STATUS_INTERNAL;
    for(unsigned i=0;i<32;++i) {filename[2*i]=hex[digest[i]>>4];filename[2*i+1]=hex[digest[i]&15];}
    filename[64]=0;return PXA_STATUS_OK;
}
static pxa_status_t permission_directory(pxa_esp_app_storage_t *storage,int create,int *directory) {
    const char *root;char path[PXA_POSIX_APP_PATH_BYTES];*directory=-1;
    pxa_status_t status=pxa_esp_app_storage_root(storage,&root);if(status!=PXA_STATUS_OK)return status;
    int n=snprintf(path,sizeof(path),"%s/.pxa-policy",root);
    if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
    if(create && mkdir(path,0700)!=0 && errno!=EEXIST)return io_error();
    *directory=pxa_esp_open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    return *directory>=0?PXA_STATUS_OK:errno==ENOENT?PXA_STATUS_NOT_FOUND:io_error();
}
static pxa_status_t load_permission(void *context,pxa_bytes_t identity,pxa_bytes_t name,
    pxa_bytes_t scope,pxa_permission_decision_t *decision) {
    pxa_esp_app_storage_t *storage=context;uint8_t digest[32],tag[32];char filename[65];int directory=-1,file=-1;
    if(!storage || !decision)return PXA_STATUS_INVALID_ARGUMENT;
    *decision=PXA_PERMISSION_DENY;
    pxa_status_t status=permission_key(storage,identity,name,scope,digest,filename);
    if(status==PXA_STATUS_OK)status=permission_directory(storage,0,&directory);
    if(status!=PXA_STATUS_OK)return status;
    file=pxa_esp_openat(directory,filename,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(file<0)status=errno==ENOENT?PXA_STATUS_NOT_FOUND:io_error();
    else {
        struct stat st;
        if(pxa_esp_fstat(file,&st)!=0)status=io_error();
        else if(!S_ISREG(st.st_mode) || st.st_nlink>1 || st.st_size!=32)status=PXA_STATUS_DENIED;
        else {
            size_t done=0;
            while(done<sizeof(tag)) {
                ssize_t n=read(file,tag+done,sizeof(tag)-done);
                if(n<0 && errno==EINTR)continue;
                if(n<=0) {status=PXA_STATUS_IO_ERROR;break;}
                done+=(size_t)n;
            }
            if(status==PXA_STATUS_OK)status=pxa_esp_record_verify(
                (pxa_bytes_t){permission_purpose,sizeof(permission_purpose)-1},
                (pxa_bytes_t){storage->authority_binding,32},(pxa_bytes_t){digest,32},tag);
        }
        if(pxa_esp_close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
    }
    if(pxa_esp_close(directory)!=0 && status==PXA_STATUS_OK)status=io_error();
    if(status==PXA_STATUS_OK)*decision=PXA_PERMISSION_ALLOW;
    return status;
}
static pxa_status_t save_permission(void *context,pxa_bytes_t identity,pxa_bytes_t name,
    pxa_bytes_t scope,pxa_permission_decision_t decision) {
    pxa_esp_app_storage_t *storage=context;uint8_t digest[32],tag[32];char filename[65],pending[74];int directory=-1,file=-1;
    if(!storage || (decision!=PXA_PERMISSION_ALLOW && decision!=PXA_PERMISSION_DENY))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=permission_key(storage,identity,name,scope,digest,filename);
    if(status==PXA_STATUS_OK && decision==PXA_PERMISSION_ALLOW)status=pxa_esp_record_seal(
        (pxa_bytes_t){permission_purpose,sizeof(permission_purpose)-1},
        (pxa_bytes_t){storage->authority_binding,32},(pxa_bytes_t){digest,32},tag);
    if(status==PXA_STATUS_OK)status=permission_directory(storage,decision==PXA_PERMISSION_ALLOW,&directory);
    if(status==PXA_STATUS_NOT_FOUND && decision==PXA_PERMISSION_DENY)return PXA_STATUS_OK;
    if(status!=PXA_STATUS_OK)return status;
    if(decision==PXA_PERMISSION_DENY) {
        if(pxa_esp_unlinkat(directory,filename,0)!=0 && errno!=ENOENT)status=io_error();
    } else {
        snprintf(pending,sizeof(pending),".%s.pending",filename);
        if(pxa_esp_unlinkat(directory,pending,0)!=0 && errno!=ENOENT)status=io_error();
        if(status==PXA_STATUS_OK) {
            file=pxa_esp_openat(directory,pending,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(file<0)status=io_error();
        }
        if(status==PXA_STATUS_OK) {
            size_t done=0;
            while(done<sizeof(tag)) {
                ssize_t n=write(file,tag+done,sizeof(tag)-done);
                if(n<0 && errno==EINTR)continue;
                if(n<=0) {status=io_error();break;}
                done+=(size_t)n;
            }
            if(status==PXA_STATUS_OK && pxa_esp_fsync(file)!=0)status=io_error();
        }
        if(file>=0 && pxa_esp_close(file)!=0 && status==PXA_STATUS_OK)status=io_error();
        if(status==PXA_STATUS_OK && pxa_esp_renameat(directory,pending,directory,filename)!=0)status=io_error();
        if(status!=PXA_STATUS_OK)pxa_esp_unlinkat(directory,pending,0);
    }
    if(status==PXA_STATUS_OK && pxa_esp_fsync(directory)!=0)status=io_error();
    if(pxa_esp_close(directory)!=0 && status==PXA_STATUS_OK)status=io_error();
    return status;
}
pxa_status_t pxa_esp_app_storage_permissions(pxa_esp_app_storage_t *storage,pxa_permission_store_t *store) {
    if(!storage || !store)return PXA_STATUS_INVALID_ARGUMENT;
    *store=(pxa_permission_store_t){sizeof(*store),storage,load_permission,save_permission};return PXA_STATUS_OK;
}
static pxa_status_t transfer_ready(pxa_esp_app_storage_t *source,pxa_esp_app_storage_t *destination) {
    if(!source || !destination)return PXA_STATUS_INVALID_ARGUMENT;
    if(strcmp(source->identity,destination->identity) ||
        memcmp(source->install_id,destination->install_id,sizeof(source->install_id)) ||
        source->data_generation!=destination->data_generation)return PXA_STATUS_DENIED;
    if(!source->ready || !destination->ready)return PXA_STATUS_BAD_STATE;
    pxa_status_t status=ready(source);
    if(status==PXA_STATUS_OK)status=ready(destination);
    return status;
}
pxa_status_t pxa_esp_app_storage_import_legacy_permissions(pxa_esp_app_storage_t *storage,
    pxa_bytes_t identity,const pxa_permission_store_t *source,
    const pxa_package_permission_t *permissions,size_t count) {
    if(!storage || !source || source->struct_size<sizeof(*source) || !source->load ||
        (count && !permissions) || count>UINT16_MAX)return PXA_STATUS_INVALID_ARGUMENT;
    if(!identity.data || identity.size!=strlen(storage->identity) ||
        memcmp(identity.data,storage->identity,identity.size))return PXA_STATUS_DENIED;
    pxa_status_t status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    uint32_t checkpoint=0;
    for(size_t i=0;i<count;++i) {
        pxa_esp_posix_io_checkpoint(&checkpoint);
        pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
        status=source->load(source->context,identity,permissions[i].name,permissions[i].scope,&decision);
        if(status==PXA_STATUS_NOT_FOUND) {status=PXA_STATUS_OK;decision=PXA_PERMISSION_DENY;}
        if(status!=PXA_STATUS_OK)return status;
        if(decision!=PXA_PERMISSION_ALLOW && decision!=PXA_PERMISSION_DENY)return PXA_STATUS_DENIED;
        status=save_permission(storage,identity,permissions[i].name,permissions[i].scope,decision);
        if(status!=PXA_STATUS_OK)return status;
    }
    return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_app_storage_transfer_permissions(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,const pxa_package_permission_t *permissions,size_t count) {
    if(count && !permissions)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=transfer_ready(source,destination);
    if(status!=PXA_STATUS_OK || source==destination)return status;
    pxa_bytes_t identity={(const uint8_t *)source->identity,strlen(source->identity)};
    for(size_t i=0;i<count;++i) {
        pxa_permission_decision_t decision=PXA_PERMISSION_DENY;
        status=load_permission(source,identity,permissions[i].name,permissions[i].scope,&decision);
        if(status==PXA_STATUS_NOT_FOUND)decision=PXA_PERMISSION_DENY;
        else if(status!=PXA_STATUS_OK)return status;
        status=save_permission(destination,identity,permissions[i].name,permissions[i].scope,decision);
        if(status!=PXA_STATUS_OK)return status;
    }
    return PXA_STATUS_OK;
}
static const uint8_t grants_purpose[]="pxa.directory.grants.v1";
pxa_status_t pxa_esp_app_storage_seal_grants(pxa_esp_app_storage_t *storage,pxa_bytes_t body,uint8_t tag[32]) {
    if(!storage || !tag)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    return pxa_esp_record_seal((pxa_bytes_t){grants_purpose,sizeof(grants_purpose)-1},
        (pxa_bytes_t){storage->authority_binding,32},body,tag);
}
pxa_status_t pxa_esp_app_storage_verify_grants(pxa_esp_app_storage_t *storage,pxa_bytes_t body,const uint8_t tag[32]) {
    if(!storage || !tag)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=ready(storage);if(status!=PXA_STATUS_OK)return status;
    return pxa_esp_record_verify((pxa_bytes_t){grants_purpose,sizeof(grants_purpose)-1},
        (pxa_bytes_t){storage->authority_binding,32},body,tag);
}
static pxa_status_t transfer_tag(pxa_esp_app_storage_t *source,pxa_esp_app_storage_t *destination,
    pxa_bytes_t purpose,pxa_bytes_t body,const uint8_t source_tag[32],uint8_t destination_tag[32]) {
    if(!source_tag || !destination_tag || (!body.data && body.size))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=transfer_ready(source,destination);
    if(status==PXA_STATUS_OK)status=pxa_esp_record_verify(purpose,
        (pxa_bytes_t){source->authority_binding,32},body,source_tag);
    if(status==PXA_STATUS_OK)status=pxa_esp_record_seal(purpose,
        (pxa_bytes_t){destination->authority_binding,32},body,destination_tag);
    return status;
}
pxa_status_t pxa_esp_app_storage_transfer_permission_tag(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,const uint8_t digest[32],const uint8_t source_tag[32],uint8_t destination_tag[32]) {
    if(!digest)return PXA_STATUS_INVALID_ARGUMENT;
    return transfer_tag(source,destination,(pxa_bytes_t){permission_purpose,sizeof(permission_purpose)-1},
        (pxa_bytes_t){digest,32},source_tag,destination_tag);
}
pxa_status_t pxa_esp_app_storage_transfer_grants(pxa_esp_app_storage_t *source,
    pxa_esp_app_storage_t *destination,pxa_bytes_t body,const uint8_t source_tag[32],uint8_t destination_tag[32]) {
    return transfer_tag(source,destination,(pxa_bytes_t){grants_purpose,sizeof(grants_purpose)-1},body,source_tag,destination_tag);
}

pxa_status_t pxa_esp_app_storage_close(pxa_esp_app_storage_t **owner) {
    pxa_status_t status=PXA_STATUS_OK,next;pxa_esp_app_storage_t *storage;
    if(!owner)return PXA_STATUS_INVALID_ARGUMENT;
    if(!*owner)return PXA_STATUS_OK;
    storage=*owner;
    if(storage->mounted) {
        esp_err_t result=esp_vfs_littlefs_unregister_blockdev(storage->image);
        if(result!=ESP_OK && esp_littlefs_blockdev_mounted(storage->image))return PXA_STATUS_IO_ERROR;
        storage->mounted=0;
    }
    if(storage->alias_bound) {
        status=pxa_esp_volumes_unbind_private(storage->alias,storage->epoch);storage->alias_bound=0;
    }
    if(storage->image) {
        next=pxa_esp_image_blockdev_close(&storage->image);if(status==PXA_STATUS_OK)status=next;
    }
    if(storage->home_bound) {
        next=pxa_esp_volumes_unbind_private(storage->home,storage->epoch);if(status==PXA_STATUS_OK)status=next;
    }
    if(storage->media_epoch) {
        next=pxa_esp_volumes_release(storage->media_epoch);if(status==PXA_STATUS_OK)status=next;
    }
    heap_caps_free(storage);*owner=NULL;return status;
}
