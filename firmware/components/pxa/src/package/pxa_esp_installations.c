#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_installations.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_installations_internal.h"
#include "pxa_esp_transfer_receipt.h"
#include "pxa_esp_package_trust.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_publisher_directory.h"
#include "pxa_esp_volumes.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define openat pxa_esp_openat
#define close pxa_esp_close
#define fstat pxa_esp_fstat
#define fstatat pxa_esp_fstatat
#define mkdirat pxa_esp_mkdirat
#define fsync pxa_esp_fsync
#endif

#ifndef CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES
#define CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES 65536
#endif
#ifndef CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES
#define CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES 8192
#endif
static void *external_allocate(size_t bytes) {
    /* These are cold control-plane owners/scratch, never DMA buffers. Do not
     * silently consume scarce SRAM if a board cannot honor this policy. */
    return heap_caps_calloc(1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
}
uint64_t pxa_esp_installations_private_capacity(void) {
    /* Two copies for atomic FS replacement, two KV slots plus compaction and
     * bounded metadata headroom. This is logical capacity; sparse backing
     * materializes only the extents actually programmed by LittleFS. */
    uint64_t bytes=UINT64_C(2)*CONFIG_PXA_PRIVATE_FS_QUOTA_BYTES+
        UINT64_C(4)*CONFIG_PXA_PRIVATE_KV_QUOTA_BYTES+65536;
    return (bytes+4095)&~UINT64_C(4095);
}
uint64_t pxa_esp_installations_private_initial_bytes(void) {
    /* Logical quotas retain replacement headroom; only one 64-KiB extent and
     * its descriptor are materialized for an empty private filesystem. */
    return UINT64_C(65536)+4096;
}
static pxa_status_t authenticated_tag(void *context,pxa_bytes_t purpose,pxa_bytes_t binding,
    pxa_bytes_t body,uint8_t tag[32]) {
    (void)context;return pxa_esp_record_seal(purpose,binding,body,tag);
}
static pxa_status_t available(pxa_esp_installations_t *owner) {
    pxa_volume_info_t volume;
    if(!owner)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_storage_volume(owner->volume.mount_epoch,&volume);
    if(status!=PXA_STATUS_OK)return status;
    return volume.state==PXA_VOLUME_READY && !volume.ambiguous?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;
}
/* Even a damaged receipt quarantines its home. Only the native resumer may
 * authenticate it and decide whether to publish or discard the target. */
static pxa_status_t visible(pxa_esp_installations_t *owner,const char *locator) {
    if(!locator || !pxa_fs_scoped_path_is_valid((pxa_bytes_t){(const uint8_t *)locator,strlen(locator)}))
        return PXA_STATUS_INVALID_ARGUMENT;
    char path[PXA_POSIX_APP_PATH_BYTES];
    pxa_status_t mapped=pxa_esp_publisher_path(owner->volume.mount_epoch,"apps",locator,path,sizeof(path));
    if(mapped!=PXA_STATUS_OK)return mapped;
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)return errno==ENOENT?PXA_STATUS_OK:PXA_STATUS_DENIED;
    const char *names[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1,
        PXA_ESP_TRANSFER_SLOT_0 ".pending",PXA_ESP_TRANSFER_SLOT_1 ".pending"};
    pxa_status_t status=PXA_STATUS_OK;
    for(unsigned i=0;i<4;++i) {
        struct stat metadata;
        if(fstatat(directory,names[i],&metadata,AT_SYMLINK_NOFOLLOW)==0) {status=PXA_STATUS_BUSY;break;}
        if(errno!=ENOENT) {status=PXA_STATUS_DENIED;break;}
    }
    if(close(directory)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    return status;
}
typedef struct {pxa_esp_installations_t *owner;pxa_posix_app_home_visitor_fn visit;void *context;} visible_visit_t;
static int visit_visible(void *context,const pxa_posix_app_home_t *home) {
    visible_visit_t *scan=context;
    return visible(scan->owner,home->locator)==PXA_STATUS_OK?scan->visit(scan->context,home):1;
}
static pxa_status_t check_space(void *context,const char *home,
    const pxa_package_manifest_t *manifest,size_t manifest_bytes,uint8_t staged) {
    pxa_esp_installations_t *owner=context;(void)home;(void)manifest;(void)manifest_bytes;(void)staged;
    if(!owner->plan)return PXA_STATUS_DENIED;
    pxa_esp_volumes_lock();
    pxa_status_t status=pxa_install_plan_validate(pxa_esp_volumes_registry(),owner->plan);
    pxa_esp_volumes_unlock();return status;
}
static pxa_status_t prepare_home(void *context,const pxa_posix_app_home_t *home) {
    pxa_esp_installations_t *owner=context;const char *root;
    pxa_status_t status=pxa_esp_app_storage_close(&owner->preparing_storage);
    if(owner->preparing_storage)return status;
    if(owner->resetting_data) {
        /* The authenticated record still references the previous generation.
         * Only this never-committed destination may be discarded on retry. */
        char candidate[PXA_POSIX_APP_PATH_BYTES];
        status=pxa_posix_app_home_path(home,PXA_POSIX_APP_HOME_PRIVATE,candidate,sizeof(candidate));
        if(status!=PXA_STATUS_OK)return status;
        status=pxa_posix_fs_remove_tree(candidate);
        if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND)return status;
    }
    status=pxa_esp_app_storage_create(home,owner->volume.mount_epoch,pxa_esp_installations_private_capacity(),owner->plan,&owner->preparing_storage);
    if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_root(owner->preparing_storage,&root);
    pxa_status_t closed=pxa_esp_app_storage_close(&owner->preparing_storage);
    return status==PXA_STATUS_OK?closed:status;
}
static pxa_status_t map_locator(void *context,const char *locator,uint8_t reverse,char *out,size_t bytes) {
    pxa_esp_installations_t *owner=context;
    return pxa_esp_publisher_map(owner->root,&owner->volume.id,"apps",locator,reverse,out,bytes);
}
pxa_status_t pxa_esp_installations_open(uint64_t epoch,pxa_esp_installations_t **output) {
    if(!output || !epoch)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;pxa_status_t status=pxa_esp_volumes_retain(epoch);
    if(status!=PXA_STATUS_OK)return status;
    pxa_esp_installations_t *owner=external_allocate(sizeof(*owner));
    if(!owner) {pxa_esp_volumes_release(epoch);return PXA_STATUS_RESOURCE_LIMIT;}
    status=pxa_esp_storage_volume(epoch,&owner->volume);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume_path(epoch,owner->root,sizeof(owner->root));
    if(status==PXA_STATUS_OK) {
        size_t length=strlen(owner->root);
        if(length+sizeof("/PXA")>sizeof(owner->root))status=PXA_STATUS_LIMIT_EXCEEDED;
        else memcpy(owner->root+length,"/PXA",sizeof("/PXA"));
    }
    owner->config.root=owner->root;owner->config.volume_id=owner->volume.id;
    owner->config.locator_context=owner;owner->config.map_locator=map_locator;
    if(status==PXA_STATUS_OK)status=pxa_esp_package_installer_profile(&owner->config.installer,&owner->trust);
    if(status==PXA_STATUS_OK) {
        owner->config.installer.space_context=owner;owner->config.installer.check_space=check_space;
        owner->config.record_tag=authenticated_tag;
        owner->config.prepare_context=owner;owner->config.prepare_home=prepare_home;
        owner->config.installer_workspace_bytes=pxa_posix_app_store_workspace_size(&owner->config);
        if(!owner->config.installer_workspace_bytes)status=PXA_STATUS_INVALID_ARGUMENT;
        else {
            owner->config.installer_workspace=external_allocate(owner->config.installer_workspace_bytes);
            if(!owner->config.installer_workspace)status=PXA_STATUS_RESOURCE_LIMIT;
        }
    }
    if(status!=PXA_STATUS_OK) {
        heap_caps_free(owner->config.installer_workspace);heap_caps_free(owner);
        pxa_esp_volumes_release(epoch);return status;
    }
    *output=owner;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_installations_close(pxa_esp_installations_t **output) {
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    if(!*output)return PXA_STATUS_OK;
    pxa_esp_installations_t *owner=*output;
    pxa_status_t status=pxa_esp_app_storage_close(&owner->preparing_storage);
    if(owner->preparing_storage)return status;
    pxa_status_t media_status=pxa_esp_installations_close(&owner->media_owner);
    if(status==PXA_STATUS_OK)status=media_status;
    if(owner->media_owner)return status;
    *output=NULL;
    pxa_esp_volumes_release(owner->volume.mount_epoch);
    heap_caps_free(owner->config.installer_workspace);heap_caps_free(owner);
    return status;
}
pxa_status_t pxa_esp_installations_visit(pxa_esp_installations_t *owner,
    pxa_posix_app_home_visitor_fn visitor,void *context,size_t *invalid) {
    if(!visitor)return PXA_STATUS_INVALID_ARGUMENT;
    visible_visit_t scan={owner,visitor,context};
    pxa_status_t status=available(owner);
    return status==PXA_STATUS_OK?pxa_posix_app_store_visit(&owner->config,visit_visible,&scan,invalid):status;
}
pxa_status_t pxa_esp_installations_inspect(pxa_esp_installations_t *owner,const char *source,
    pxa_posix_installer_result_t *result) {
    pxa_status_t status=available(owner);pxa_posix_installer_t *installer=NULL;
    if(status!=PXA_STATUS_OK)return status;
    pxa_posix_installer_config_t config=owner->config.installer;config.storage_root=owner->root;
    status=pxa_posix_installer_init(owner->config.installer_workspace,owner->config.installer_workspace_bytes,&config,&installer);
    if(status==PXA_STATUS_OK)status=pxa_posix_installer_read_signed_manifest(installer,source,result);
    if(installer)pxa_posix_installer_deinit(installer);
    return status;
}
pxa_status_t pxa_esp_installations_locator(const uint8_t publisher[32],const char *app,char *out,size_t bytes) {
    static const char hex[]="0123456789abcdef";
    if(!publisher || !app || !out || !bytes)return PXA_STATUS_INVALID_ARGUMENT;
    out[0]=0;size_t length=strnlen(app,65);
    if(!length || length>64 || app[0]<'a' || app[0]>'z')return PXA_STATUS_INVALID_ARGUMENT;
    for(size_t i=0;i<length;++i)
        if(!((app[i]>='a' && app[i]<='z') || (app[i]>='0' && app[i]<='9') ||
            app[i]=='-' || app[i]=='_' || app[i]=='.'))return PXA_STATUS_INVALID_ARGUMENT;
    if(bytes<64+1+length+1)return PXA_STATUS_LIMIT_EXCEEDED;
    for(unsigned i=0;i<32;++i) {out[i*2]=hex[publisher[i]>>4];out[i*2+1]=hex[publisher[i]&15];}
    out[64]='/';memcpy(out+65,app,length+1);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_installations_media_path(const pxa_install_record_t *record,char *out,size_t capacity) {
    static const char hex[]="0123456789abcdef";char locator[PXA_POSIX_APP_LOCATOR_BYTES],digest[65],physical[PXA_POSIX_APP_PATH_BYTES],mount[PXA_ESP_STORAGE_PATH_BYTES];
    if(!record || !out || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    out[0]=0;
    if(!(record->flags&PXA_INSTALL_RECORD_HAS_MEDIA))return PXA_STATUS_DENIED;
    pxa_status_t status=pxa_esp_installations_locator(record->publisher_root,record->app_id,locator,sizeof(locator));
    if(status!=PXA_STATUS_OK)return status;
    pxa_volume_info_t volume;
    status=pxa_esp_storage_find_volume(&record->media_volume,&volume);
    if(status==PXA_STATUS_OK)status=pxa_esp_publisher_path(volume.mount_epoch,"media",locator,physical,sizeof(physical));
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume_path(volume.mount_epoch,mount,sizeof(mount));
    if(status!=PXA_STATUS_OK)return status;
    const char *relative=physical+strlen(mount)+1;
    if(!(record->flags&PXA_INSTALL_RECORD_MANAGED_MEDIA)) {
        if(memcmp(record->media_set_sha256,(uint8_t[32]){0},32))return PXA_STATUS_DENIED;
        int n=snprintf(out,capacity,"%s",relative);
        return n<0 || (size_t)n>=capacity?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
    }
    if(!memcmp(record->media_set_sha256,(uint8_t[32]){0},32))return PXA_STATUS_DENIED;
    for(unsigned i=0;i<32;++i) {digest[i*2]=hex[record->media_set_sha256[i]>>4];digest[i*2+1]=hex[record->media_set_sha256[i]&15];}
    digest[64]=0;
    int n=snprintf(out,capacity,"%s/%s/package",relative,digest);
    return n<0 || (size_t)n>=capacity?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
/* Walk every generation ancestor without links, including read-only loads.
 * Only the installer writes the package subdirectory. Sync each newly made
 * parent so losing power cannot publish code with an undurable resource path. */
static pxa_status_t resource_directory(pxa_esp_installations_t *owner,uint8_t create) {
    const char *relative=owner->resource_root+strlen(owner->root)+1;
    int current=open(owner->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(current<0)return PXA_STATUS_UNAVAILABLE;
    pxa_status_t status=PXA_STATUS_OK;
    while(*relative) {
        const char *slash=strchr(relative,'/');size_t size=slash?(size_t)(slash-relative):strlen(relative);
        char name[65];
        if(!size || size>=sizeof(name)) {status=PXA_STATUS_DENIED;break;}
        memcpy(name,relative,size);name[size]=0;
        if(create && mkdirat(current,name,0700)!=0 && errno!=EEXIST) {status=PXA_STATUS_IO_ERROR;break;}
        if(create && fsync(current)!=0) {status=PXA_STATUS_IO_ERROR;break;}
        int next=openat(current,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0) {status=errno==ENOENT?PXA_STATUS_NOT_FOUND:PXA_STATUS_DENIED;break;}
        close(current);current=next;
        if(!slash)break;
        relative=slash+1;
    }
    if(close(current)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    return status;
}
static pxa_status_t resource_options(pxa_esp_installations_t *owner,const pxa_install_record_t *record,
    uint8_t create,pxa_posix_installer_config_t *config) {
    char relative[256];pxa_status_t status=available(owner);
    if(status!=PXA_STATUS_OK)return status;
    if(!record || !(record->flags&PXA_INSTALL_RECORD_MANAGED_MEDIA) ||
        memcmp(record->media_volume.bytes,owner->volume.id.bytes,16))return PXA_STATUS_DENIED;
    if(create && !(owner->volume.capabilities&PXA_VOLUME_WRITE))return PXA_STATUS_DENIED;
    status=pxa_esp_installations_media_path(record,relative,sizeof(relative));if(status!=PXA_STATUS_OK)return status;
    int n=snprintf(owner->resource_package,sizeof(owner->resource_package),"%s/%s",owner->root,relative+4);
    if(n<0 || (size_t)n>=sizeof(owner->resource_package))return PXA_STATUS_LIMIT_EXCEEDED;
    strcpy(owner->resource_root,owner->resource_package);
    *strrchr(owner->resource_root,'/')=0;
    *config=owner->config.installer;config->storage_root=owner->resource_root;
    config->flags|=PXA_POSIX_INSTALLER_FLAG_APP_HOME|PXA_POSIX_INSTALLER_FLAG_COMPOSITE_IDENTITY;
    config->flags&=~(PXA_POSIX_INSTALLER_FLAG_RETAIN_PREVIOUS|PXA_POSIX_INSTALLER_FLAG_REPAIR_CORRUPT);
    config->expected_manifest_sha256=record->media_set_sha256;
    config->expected_package_kind=PXA_PACKAGE_KIND_RESOURCES;
    return PXA_STATUS_OK;
}
static int owned_stream_source(pxa_esp_installations_t *owner,const char *source) {
    size_t root_bytes=strlen(owner->config.root);
    static const char prefix[]="/cache/store/.stream-";
    if(!source || strncmp(source,owner->config.root,root_bytes) ||
        strncmp(source+root_bytes,prefix,sizeof(prefix)-1))return 0;
    const char *name=source+root_bytes+sizeof(prefix)-1;
    if(strlen(name)!=8)return 0;
    for(size_t i=0;i<8;++i)if(!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f')))return 0;
    return 1;
}
static pxa_status_t resources_operation(pxa_esp_installations_t *owner,const pxa_install_record_t *record,
    const char *source,const pxa_install_plan_t *plan,pxa_posix_installer_result_t *result,int metadata_only) {
    if(!owner || !record || !result)return PXA_STATUS_INVALID_ARGUMENT;
    owner->resource_verified=0;owner->resource_loaded=0;
    if(source) {
        if(!plan || (plan->app_mount_epoch!=owner->volume.mount_epoch &&
            plan->media_mount_epoch!=owner->volume.mount_epoch))return PXA_STATUS_INVALID_ARGUMENT;
        pxa_esp_volumes_lock();pxa_status_t admitted=pxa_install_plan_validate(pxa_esp_volumes_registry(),plan);pxa_esp_volumes_unlock();
        if(admitted!=PXA_STATUS_OK)return admitted;
    }
    pxa_posix_installer_config_t config;pxa_posix_installer_t *installer=NULL;
    pxa_status_t status=resource_options(owner,record,source!=NULL,&config);
    if(status==PXA_STATUS_OK && (!result->root || result->root_capacity<=strlen(owner->resource_package)))
        status=PXA_STATUS_RESOURCE_LIMIT;
    if(status==PXA_STATUS_OK)status=resource_directory(owner,source!=NULL);
    if(status==PXA_STATUS_OK)status=pxa_posix_installer_init(owner->config.installer_workspace,
        owner->config.installer_workspace_bytes,&config,&installer);
    if(status==PXA_STATUS_OK) {
        pxa_posix_installer_identity_t identity={{record->publisher_root,32},
            {(const uint8_t *)record->app_id,strlen(record->app_id)}};
        owner->plan=plan;
        if(source)status=owned_stream_source(owner,source)
            ?pxa_posix_installer_adopt_directory(installer,source,&identity,result,NULL)
            :pxa_posix_installer_install_for_identity(installer,source,&identity,result,NULL);
        else status=metadata_only
            ?pxa_posix_installer_read_signed_manifest(installer,owner->resource_package,result)
            :pxa_posix_installer_verify_source(installer,owner->resource_package,result);
        owner->plan=NULL;
        if(status==PXA_STATUS_OK) {
            const pxa_package_manifest_t *manifest=*result->manifest;
            if(!manifest || manifest->app_id.size!=identity.app_id.size ||
                memcmp(manifest->app_id.data,identity.app_id.data,identity.app_id.size) ||
                memcmp(manifest->management_key_id,record->publisher_root,32))status=PXA_STATUS_DENIED;
        }
    }
    if(installer)pxa_posix_installer_deinit(installer);
    if(status==PXA_STATUS_OK) {
        owner->resource_loaded=1;owner->resource_verified=!metadata_only;
    }
    return status;
}
pxa_status_t pxa_esp_installations_install_resources(pxa_esp_installations_t *owner,
    const pxa_install_record_t *record,const char *source,const pxa_install_plan_t *plan,pxa_posix_installer_result_t *result) {
    if(!source)return PXA_STATUS_INVALID_ARGUMENT;
    return resources_operation(owner,record,source,plan,result,0);
}
pxa_status_t pxa_esp_installations_verify_resources(pxa_esp_installations_t *owner,
    const pxa_install_record_t *record,pxa_posix_installer_result_t *result) {
    return resources_operation(owner,record,NULL,NULL,result,0);
}
typedef struct {
    pxa_posix_installer_result_t result;
    pxa_package_manifest_t *manifest;
    uint8_t *encoded;void *workspace;
    char path[PXA_POSIX_APP_PATH_BYTES];
} media_check_t;
/* Manual imports retain mutable user files, not publisher-signed payload
 * authority. The authenticated record binds the exact volume and fixed App
 * namespace. Walk each ancestor without links and retain the mount; never
 * manufacture an empty directory when the original media has disappeared. */
static __attribute__((noinline)) pxa_status_t verify_manual_media(pxa_esp_installations_t *owner,const pxa_install_record_t *record,
    const pxa_volume_info_t *volume) {
    char relative[256],publisher[65];
    pxa_status_t status=pxa_esp_installations_media_path(record,relative,sizeof(relative));
    if(status!=PXA_STATUS_OK)return status;
    status=pxa_esp_installations_close(&owner->media_owner);
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_open(volume->mount_epoch,&owner->media_owner);
    if(status!=PXA_STATUS_OK)return status;
    const char *separator=strchr(relative+10,'/');
    if(!separator || separator==relative+10 || separator-(relative+10)>64)return PXA_STATUS_DENIED;
    size_t publisher_bytes=(size_t)(separator-(relative+10));
    memcpy(publisher,relative+10,publisher_bytes);publisher[publisher_bytes]=0;
    const char *names[]={"media",publisher,record->app_id};
    int directory=open(owner->media_owner->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0)status=PXA_STATUS_UNAVAILABLE;
    for(unsigned i=0;status==PXA_STATUS_OK && i<3;++i) {
        int next=openat(directory,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(next<0)status=errno==ENOENT?PXA_STATUS_UNAVAILABLE:PXA_STATUS_DENIED;
        if(close(directory)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
        directory=next;
    }
    if(directory>=0 && close(directory)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    if(status!=PXA_STATUS_OK)(void)pxa_esp_installations_close(&owner->media_owner);
    return status;
}
static pxa_status_t check_media(void *context,const pxa_install_record_t *record,int metadata_only) {
    pxa_esp_installations_t *owner=context;pxa_volume_info_t volume;
    if(!owner || !record)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_storage_find_volume(&record->media_volume,&volume);
    if(status!=PXA_STATUS_OK)return status;
    if(volume.state!=PXA_VOLUME_READY || volume.ambiguous || !(volume.capabilities&PXA_VOLUME_READ))
        return PXA_STATUS_UNAVAILABLE;
    if(!(record->flags&PXA_INSTALL_RECORD_MANAGED_MEDIA))return verify_manual_media(owner,record,&volume);
    media_check_t *check=external_allocate(sizeof(*check));if(!check)return PXA_STATUS_RESOURCE_LIMIT;
    status=pxa_esp_installations_media_path(record,check->path,sizeof(check->path));
    if(status!=PXA_STATUS_OK)goto done;
    if(owner->media_owner && owner->media_owner->volume.mount_epoch==volume.mount_epoch &&
        (owner->media_owner->resource_verified || (metadata_only && owner->media_owner->resource_loaded)) &&
        strlen(owner->media_owner->resource_package)>strlen(check->path) &&
        !strcmp(owner->media_owner->resource_package+strlen(owner->media_owner->resource_package)-strlen(check->path),check->path))goto done;
    status=pxa_esp_installations_close(&owner->media_owner);if(status!=PXA_STATUS_OK)goto done;
    status=pxa_esp_installations_open(volume.mount_epoch,&owner->media_owner);if(status!=PXA_STATUS_OK)goto done;
    pxa_posix_installer_config_t config;
    status=resource_options(owner->media_owner,record,0,&config);if(status!=PXA_STATUS_OK)goto done;
    status=resource_directory(owner->media_owner,0);if(status!=PXA_STATUS_OK)goto done;
    int directory=open(owner->media_owner->resource_package,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int file=directory>=0?openat(directory,"manifest.pxm",O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;
    if(directory>=0)close(directory);
    struct stat info;
    if(file<0 || fstat(file,&info)!=0 || !S_ISREG(info.st_mode) || info.st_nlink>1 || info.st_size<12 ||
        (uint64_t)info.st_size>PXA_ESP_SIGNED_MANIFEST_MAX_BYTES) {
        if(file>=0)close(file);
        status=PXA_STATUS_DENIED;goto done;
    }
    check->encoded=external_allocate((size_t)info.st_size);
    if(!check->encoded) {close(file);status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    size_t offset=0;
    while(offset<(size_t)info.st_size) {
        ssize_t count=read(file,check->encoded+offset,(size_t)info.st_size-offset);
        if(count<0 && errno==EINTR)continue;
        if(count<=0) {status=PXA_STATUS_IO_ERROR;break;}
        offset+=(size_t)count;
    }
    if(close(file)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    if(status!=PXA_STATUS_OK)goto done;
    pxa_package_limits_t limits;
    status=pxa_package_manifest_measure((pxa_bytes_t){check->encoded,offset},&config.limits,&limits);
    if(status!=PXA_STATUS_OK)goto done;
    size_t bytes=pxa_package_manifest_workspace_size(&limits);
    check->workspace=external_allocate(bytes);
    if(!check->workspace) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    check->result=(pxa_posix_installer_result_t){sizeof(check->result),check->workspace,bytes,
        check->encoded,offset,&check->manifest,check->path,sizeof(check->path)};
    status=resources_operation(owner->media_owner,record,NULL,NULL,&check->result,metadata_only);
done:
    if(status!=PXA_STATUS_OK)(void)pxa_esp_installations_close(&owner->media_owner);
    heap_caps_free(check->workspace);heap_caps_free(check->encoded);heap_caps_free(check);return status;
}
pxa_status_t pxa_esp_installations_verify_media(void *context,const pxa_install_record_t *record) {
    return check_media(context,record,0);
}
static pxa_status_t load_media(void *context,const pxa_install_record_t *record) {
    return check_media(context,record,1);
}
static pxa_status_t add_file(const pxa_volume_info_t *volume,uint64_t size,uint64_t *total) {
    uint64_t rounded;pxa_status_t status=pxa_install_file_allocation(volume,size,&rounded);
    if(status!=PXA_STATUS_OK)return status;
    if(*total>UINT64_MAX-rounded)return PXA_STATUS_LIMIT_EXCEEDED;
    *total+=rounded;return PXA_STATUS_OK;
}
static pxa_status_t plan_contents(pxa_esp_installations_t *owner,
    const pxa_package_manifest_t *manifest,uint64_t transfer,uint8_t new_private,uint8_t prepared,pxa_install_plan_t *output) {
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    memset(output,0,sizeof(*output));pxa_status_t status=available(owner);pxa_volume_info_t volume;
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_refresh_volume(owner->volume.mount_epoch);
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume(owner->volume.mount_epoch,&volume);
    if(status!=PXA_STATUS_OK)return status;
    uint64_t bytes=0,directories=manifest?16:8;
    for(uint16_t i=0;manifest && i<manifest->file_count;++i) {
        if(!prepared)status=add_file(&volume,manifest->files[i].size,&bytes);
        if(status!=PXA_STATUS_OK)return status;
        for(size_t j=0;j<manifest->files[i].path.size;++j)if(manifest->files[i].path.data[j]=='/')++directories;
    }
    if(manifest && !prepared) {
        status=add_file(&volume,manifest->encoded.size,&bytes);
        if(status==PXA_STATUS_OK)status=add_file(&volume,PXA_POSIX_INSTALLER_SIGNATURE_ENVELOPE_BYTES,&bytes);
    }
    if(status==PXA_STATUS_OK && new_private && !(volume.capabilities&PXA_VOLUME_PRIVATE_NATIVE))
        status=add_file(&volume,pxa_esp_installations_private_initial_bytes(),&bytes);
    if(status!=PXA_STATUS_OK)return status;
    uint64_t metadata=directories*volume.allocation_unit;
    if(bytes>UINT64_MAX-metadata)return PXA_STATUS_LIMIT_EXCEEDED;
    pxa_install_requirements_t requirements={bytes+metadata,0,0,0,4096};
    status=add_file(&volume,transfer,&requirements.app_temporary_bytes);
    return status==PXA_STATUS_OK?pxa_esp_storage_plan(&requirements,owner->volume.mount_epoch,0,output):status;
}
pxa_status_t pxa_esp_installations_plan(pxa_esp_installations_t *owner,
    const pxa_package_manifest_t *manifest,uint64_t transfer,uint8_t new_private,pxa_install_plan_t *output) {
    if(!manifest)return PXA_STATUS_INVALID_ARGUMENT;
    return plan_contents(owner,manifest,transfer,new_private,0,output);
}
pxa_status_t pxa_esp_installations_plan_prepared(pxa_esp_installations_t *owner,
    const pxa_package_manifest_t *manifest,uint8_t new_private,pxa_install_plan_t *output) {
    if(!manifest)return PXA_STATUS_INVALID_ARGUMENT;
    return plan_contents(owner,manifest,0,new_private,1,output);
}
pxa_status_t pxa_esp_installations_plan_archive(pxa_esp_installations_t *owner,
    uint8_t new_private,pxa_install_plan_t *output) {
    return plan_contents(owner,NULL,0,new_private,0,output);
}
pxa_status_t pxa_esp_installations_install(pxa_esp_installations_t *owner,const char *source,
    const char *locator,const pxa_install_record_t *approved,uint64_t generation,
    const uint8_t digest[32],const pxa_install_plan_t *plan,pxa_posix_media_ready_fn media,void *context,
    pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    if(!plan || !digest || !approved || !owner || plan->app_mount_epoch!=owner->volume.mount_epoch)
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,locator);if(status!=PXA_STATUS_OK)return status;
    pxa_esp_volumes_lock();status=pxa_install_plan_validate(pxa_esp_volumes_registry(),plan);pxa_esp_volumes_unlock();
    if(status!=PXA_STATUS_OK)return status;
    char expected_locator[PXA_POSIX_APP_LOCATOR_BYTES];
    status=pxa_esp_installations_locator(approved->publisher_root,approved->app_id,expected_locator,sizeof(expected_locator));
    if(status!=PXA_STATUS_OK || !locator || strcmp(expected_locator,locator))return PXA_STATUS_DENIED;
    owner->plan=plan;owner->config.media_ready=media?media:pxa_esp_installations_verify_media;owner->config.media_context=media?context:owner;
    owner->config.approved_manifest_sha256=digest;
    /* Only our private download cache transfers ownership. Ordinary file
     * manager installs retain their source and keep the copying semantics. */
    owner->config.adopt_source_directory=owned_stream_source(owner,source);
    status=pxa_posix_app_store_install(&owner->config,locator,source,generation,approved,result,home);
    owner->config.adopt_source_directory=0;
    owner->plan=NULL;owner->config.approved_manifest_sha256=NULL;
    owner->config.media_ready=NULL;owner->config.media_context=NULL;return status;
}
pxa_status_t pxa_esp_installations_load(pxa_esp_installations_t *owner,const char *locator,
    pxa_posix_media_ready_fn media,void *context,pxa_posix_installer_result_t *result,pxa_posix_app_home_t *home) {
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,locator);if(status!=PXA_STATUS_OK)return status;
    owner->config.media_ready=media?media:load_media;owner->config.media_context=media?context:owner;
    status=pxa_posix_app_store_recover(&owner->config,locator,result,home);
    owner->config.media_ready=NULL;owner->config.media_context=NULL;return status;
}
pxa_status_t pxa_esp_installations_storage(pxa_esp_installations_t *owner,
    const pxa_posix_app_home_t *home,pxa_esp_app_storage_t **storage) {
    if(!home || !(home->record.flags&PXA_INSTALL_RECORD_ENABLED) || (home->record.flags&PXA_INSTALL_RECORD_ARCHIVED))
        return PXA_STATUS_DENIED;
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,home->locator);if(status!=PXA_STATUS_OK)return status;
    status=pxa_esp_app_storage_create(home,owner->volume.mount_epoch,pxa_esp_installations_private_capacity(),NULL,storage);
    if(status==PXA_STATUS_OK && (home->record.flags&PXA_INSTALL_RECORD_HAS_MEDIA)) {
        if(!owner->media_owner)status=PXA_STATUS_UNAVAILABLE;
        else status=pxa_esp_app_storage_bind_media(*storage,owner->media_owner->volume.mount_epoch);
    }
    return status;
}
pxa_status_t pxa_esp_installations_lookup(pxa_esp_installations_t *owner,const char *locator,pxa_posix_app_home_t *home) {
    if(home)memset(home,0,sizeof(*home));
    pxa_status_t status=available(owner);
    if(status==PXA_STATUS_OK)status=visible(owner,locator);
    return status==PXA_STATUS_OK?pxa_posix_app_store_lookup(&owner->config,locator,home):status;
}
pxa_status_t pxa_esp_installations_permissions(pxa_esp_installations_t *owner,
    const pxa_posix_app_home_t *home,pxa_permission_store_t *permissions) {
    if(!permissions)return PXA_STATUS_INVALID_ARGUMENT;
    memset(permissions,0,sizeof(*permissions));
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    if(!home)return PXA_STATUS_INVALID_ARGUMENT;
    status=visible(owner,home->locator);if(status!=PXA_STATUS_OK)return status;
    if(owner->preparing_storage)return PXA_STATUS_BUSY;
    status=pxa_esp_app_storage_create(home,owner->volume.mount_epoch,pxa_esp_installations_private_capacity(),NULL,&owner->preparing_storage);
    return status==PXA_STATUS_OK?pxa_esp_app_storage_permissions(owner->preparing_storage,permissions):status;
}
pxa_status_t pxa_esp_installations_set_enabled(pxa_esp_installations_t *owner,const char *locator,
    uint64_t expected,uint8_t enabled,pxa_posix_app_home_t *home) {
    if(home)memset(home,0,sizeof(*home));
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,locator);if(status!=PXA_STATUS_OK)return status;
    return pxa_posix_app_store_set_enabled(&owner->config,locator,expected,enabled,home);
}
static pxa_status_t retire_old_data(const pxa_posix_app_home_t *home) {
    char path[PXA_POSIX_APP_PATH_BYTES],parent[PXA_POSIX_APP_PATH_BYTES];
    int n=snprintf(path,sizeof(path),"%s/private",home->home);
    if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
    pxa_status_t result=pxa_posix_fs_remove_tree(path);
    if(result==PXA_STATUS_NOT_FOUND)result=PXA_STATUS_OK;
    n=snprintf(parent,sizeof(parent),"%s/data",home->home);
    if(n<0 || (size_t)n>=sizeof(parent))return PXA_STATUS_LIMIT_EXCEEDED;
    DIR *directory=opendir(parent);
    if(!directory)return result!=PXA_STATUS_OK?result:PXA_STATUS_IO_ERROR;
    for(;;) {
        errno=0;struct dirent *entry=readdir(directory);
        if(!entry) {if(errno && result==PXA_STATUS_OK)result=PXA_STATUS_IO_ERROR;break;}
        if(strlen(entry->d_name)!=16)continue;
        uint64_t generation=0;unsigned i;
        for(i=0;i<16;++i) {
            unsigned char c=(unsigned char)entry->d_name[i];unsigned value;
            if(c>='0' && c<='9')value=c-'0';
            else if(c>='a' && c<='f')value=c-'a'+10;
            else if(c>='A' && c<='F')value=c-'A'+10;
            else break;
            generation=(generation<<4)|value;
        }
        if(i!=16 || !generation || generation>=home->record.data_generation)continue;
        n=snprintf(path,sizeof(path),"%s/%s",parent,entry->d_name);
        pxa_status_t status=(n<0 || (size_t)n>=sizeof(path))?PXA_STATUS_LIMIT_EXCEEDED:pxa_posix_fs_remove_tree(path);
        if(status!=PXA_STATUS_OK && status!=PXA_STATUS_NOT_FOUND && result==PXA_STATUS_OK)result=status;
    }
    if(closedir(directory)!=0 && result==PXA_STATUS_OK)result=PXA_STATUS_IO_ERROR;
    return result;
}
pxa_status_t pxa_esp_installations_reset_data(pxa_esp_installations_t *owner,const char *locator,
    uint64_t expected,uint64_t expected_data,pxa_posix_app_home_t *home) {
    if(!home)return PXA_STATUS_INVALID_ARGUMENT;
    memset(home,0,sizeof(*home));
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,locator);if(status!=PXA_STATUS_OK)return status;
    owner->resetting_data=1;
    status=pxa_posix_app_store_reset_data(&owner->config,locator,expected,expected_data,home);
    owner->resetting_data=0;
    if(status!=PXA_STATUS_OK)return status;
    /* Both record slots now reference the fresh root; no interrupted metadata
     * fallback can resurrect a deleted data generation. */
    return retire_old_data(home);
}
pxa_status_t pxa_esp_installations_remove(pxa_esp_installations_t *owner,const char *locator,
    uint64_t expected,uint8_t keep_data,pxa_posix_app_home_t *home) {
    if(home)memset(home,0,sizeof(*home));
    if(!home || keep_data>1)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=available(owner);if(status!=PXA_STATUS_OK)return status;
    status=visible(owner,locator);if(status!=PXA_STATUS_OK)return status;
    status=pxa_posix_app_store_archive(&owner->config,locator,expected,home);
    return status==PXA_STATUS_OK && !keep_data?pxa_posix_fs_remove_tree(home->home):status;
}
