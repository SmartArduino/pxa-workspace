#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_legacy_adoption.h"
#include "pxa_esp_installations_internal.h"
#include "pxa_esp_transfer_receipt.h"
#include "pxa_esp_permission_store.h"
#include "pxa_esp_private_authority.h"
#include "pxa_esp_private_copy.h"
#include "pxa_esp_package_limits.h"
#include "pxa_esp_volumes.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/openssl/pxa_openssl.h"
#include "pxa/fs.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
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
#define renameat pxa_esp_renameat
#define unlinkat pxa_esp_unlinkat
#define fsync pxa_esp_fsync
#define dup pxa_esp_dup
#define fdopendir pxa_esp_fdopendir
#endif
#ifndef CONFIG_PXA_STATE_ROOT
#define CONFIG_PXA_STATE_ROOT "pxa-state"
#endif
#ifndef CONFIG_PXA_PACKAGES_DIR
#define CONFIG_PXA_PACKAGES_DIR "packages"
#endif
#ifndef CONFIG_PXA_PRIVATE_DATA_DIR
#define CONFIG_PXA_PRIVATE_DATA_DIR "data"
#endif

typedef struct {
    pxa_esp_installations_t *owner;
    pxa_esp_app_storage_t *storage;
    pxa_esp_legacy_source_t source;
    pxa_esp_transfer_receipt_t receipt;
    pxa_esp_transfer_receipt_config_t journal;
    pxa_posix_app_home_t home;
    pxa_install_plan_t plan;
    pxa_package_manifest_t *manifest;
    pxa_posix_installer_result_t result;
    const pxa_esp_app_transfer_control_t *control;
    void *receipt_workspace,*manifest_workspace;
    uint8_t *encoded;
    uint64_t media_epoch;
    uint64_t repair_credit;
    int root,home_fd,data_fd,media_root;
    char mount[PXA_ESP_STORAGE_PATH_BYTES],media_mount[PXA_ESP_STORAGE_PATH_BYTES];
    char package[PXA_POSIX_APP_PATH_BYTES],result_root[PXA_POSIX_APP_PATH_BYTES];
    char scratch[PXA_POSIX_APP_PATH_BYTES],identity[130];
    uint8_t wire[PXA_INSTALL_RECORD_BYTES];
    const char *stage;
} adoption_t;
static adoption_t *pending_close;
static pxa_status_t error(void) {
    if(errno==ENOENT)return PXA_STATUS_NOT_FOUND;
    if(errno==ENOSPC || errno==EDQUOT)return PXA_STATUS_QUOTA_EXCEEDED;
    if(errno==ENOMEM || errno==EMFILE || errno==ENFILE)return PXA_STATUS_RESOURCE_LIMIT;
    if(errno==ELOOP || errno==ENOTDIR)return PXA_STATUS_DENIED;
    return PXA_STATUS_IO_ERROR;
}
static int directory(int parent,const char *name,uint8_t create) {
    if(create) {
        int made=mkdirat(parent,name,0700);
        if(made && errno!=EEXIST)return -1;
        if(!made && fsync(parent))return -1;
    }
    return openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
/* Walk every ancestor. O_NOFOLLOW on the final name alone is insufficient
 * for a journal on removable storage or POSIX test filesystems. */
static int internal_relative(const char *relative) {
    if(!relative || !*relative || *relative=='/')return 0;
    for(const char *p=relative;*p;) {
        const char *start=p;
        while(*p && *p!='/') {if((unsigned char)*p<32 || *p=='\\' || *p==127)return 0;++p;}
        size_t length=(size_t)(p-start);
        if(!length || length>=PXA_ESP_LEGACY_PATH_BYTES ||
            (length==1 && *start=='.') || (length==2 && start[0]=='.' && start[1]=='.'))return 0;
        if(*p) {++p;if(!*p)return 0;}
    }
    return 1;
}
static int walk(int root,const char *relative,uint8_t create) {
    /* These are Host control paths, including .pxa-policy. The Guest path
     * validator intentionally rejects such names, so do not use it here. */
    if(!internal_relative(relative)) {errno=EINVAL;return -1;}
    int current=dup(root);if(current<0)return -1;
    const char *p=relative;
    while(*p) {
        const char *slash=strchr(p,'/');size_t length=slash?(size_t)(slash-p):strlen(p);
        char component[PXA_ESP_LEGACY_PATH_BYTES];
        if(!length || length>=sizeof(component)) {close(current);errno=ENAMETOOLONG;return -1;}
        memcpy(component,p,length);component[length]=0;
        int next=directory(current,component,create);int saved=errno;close(current);errno=saved;
        if(next<0)return -1;
        current=next;p+=length;if(*p)++p;
    }
    return current;
}
static int parent(adoption_t *a,int root,const char *relative,const char **leaf,uint8_t create) {
    const char *slash=strrchr(relative,'/');
    if(!slash) {*leaf=relative;return dup(root);}
    size_t length=(size_t)(slash-relative);
    if(length>=sizeof(a->scratch)) {errno=ENAMETOOLONG;return -1;}
    memmove(a->scratch,relative,length);a->scratch[length]=0;*leaf=slash+1;
    return walk(root,a->scratch,create);
}
static int path_valid(const char *path,const char *prefix) {
    size_t size=strnlen(path,PXA_ESP_LEGACY_PATH_BYTES),head=strlen(prefix);
    return size<PXA_ESP_LEGACY_PATH_BYTES && size>head && !strncmp(path,prefix,head) &&
        internal_relative(path);
}
static int valid_source(const pxa_esp_legacy_source_t *s) {
    return s && s->app_epoch && s->private_present<=1 &&
        path_valid(s->package,CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PACKAGES_DIR "/") &&
        path_valid(s->private_root,CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PRIVATE_DATA_DIR "/") &&
        ((!s->media_epoch && !s->media[0]) || (s->media_epoch && path_valid(s->media,"PXA/media/")));
}
static pxa_status_t context_pack(adoption_t *a) {
    uint8_t *p=a->receipt.context;size_t size=16;
    memset(p,0,PXA_ESP_TRANSFER_CONTEXT_BYTES);memcpy(p,"PXLA",4);p[4]=1;p[5]=a->source.private_present;
    const char *paths[]={a->source.package,a->source.private_root,a->source.media};
    for(unsigned i=0;i<3;++i) {
        size_t n=strlen(paths[i]);if(n>PXA_ESP_TRANSFER_CONTEXT_BYTES-size)return PXA_STATUS_LIMIT_EXCEEDED;
        pxa_write_u16(p+8+2*i,(uint16_t)n);memcpy(p+size,paths[i],n);size+=n;
    }
    a->receipt.context_size=(uint16_t)size;return PXA_STATUS_OK;
}
static pxa_status_t context_unpack(adoption_t *a) {
    const uint8_t *p=a->receipt.context;size_t size=16;
    if(a->receipt.context_size<16 || memcmp(p,"PXLA",4) || p[4]!=1 || p[5]>1 || p[6] || p[7] || p[14] || p[15])
        return PXA_STATUS_DENIED;
    char *paths[]={a->source.package,a->source.private_root,a->source.media};a->source.private_present=p[5];
    for(unsigned i=0;i<3;++i) {
        size_t n=pxa_read_u16(p+8+2*i);
        if(n>=PXA_ESP_LEGACY_PATH_BYTES || n>a->receipt.context_size-size || memchr(p+size,0,n))return PXA_STATUS_DENIED;
        memcpy(paths[i],p+size,n);paths[i][n]=0;size+=n;
    }
    return size==a->receipt.context_size && valid_source(&a->source)?PXA_STATUS_OK:PXA_STATUS_DENIED;
}
static pxa_status_t available(adoption_t *a) {
    if(a->control && a->control->progress)a->control->progress(a->control->context,0);
    pxa_volume_info_t volume;
    pxa_status_t status=pxa_esp_storage_volume(a->owner->volume.mount_epoch,&volume);
    const uint64_t rights=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_PRIVATE_NATIVE;
    if(status!=PXA_STATUS_OK || volume.state!=PXA_VOLUME_READY || volume.ambiguous ||
        memcmp(volume.id.bytes,a->owner->volume.id.bytes,16))return PXA_STATUS_UNAVAILABLE;
    if((volume.capabilities&rights)!=rights)return PXA_STATUS_UNSUPPORTED;
    if(a->media_epoch) {
        status=pxa_esp_storage_volume(a->media_epoch,&volume);
        if(status!=PXA_STATUS_OK || volume.state!=PXA_VOLUME_READY || volume.ambiguous ||
            memcmp(volume.id.bytes,a->receipt.transfer.source.media_volume.bytes,16))return PXA_STATUS_UNAVAILABLE;
        if((volume.capabilities&(PXA_VOLUME_READ|PXA_VOLUME_WRITE))!=(PXA_VOLUME_READ|PXA_VOLUME_WRITE))return PXA_STATUS_DENIED;
    }
    return a->receipt.transfer.phase<PXA_INSTALL_TRANSFER_SURRENDERED && a->control && a->control->check?
        a->control->check(a->control->context):PXA_STATUS_OK;
}
static pxa_status_t check_copy(void *context) {return available(context);}
static pxa_status_t checkpoint(adoption_t *a,uint8_t phase) {
    if(a->control && a->control->progress)a->control->progress(a->control->context,phase);
    return available(a);
}
static pxa_status_t seal(void *context,pxa_bytes_t purpose,pxa_bytes_t binding,pxa_bytes_t body,uint8_t tag[32]) {
    pxa_esp_installations_t *owner=context;
    return owner->config.record_tag(owner->config.record_auth_context,purpose,binding,body,tag);
}
static pxa_posix_install_record_auth_t auth(adoption_t *a) {
    return (pxa_posix_install_record_auth_t){a->owner,
        {(const uint8_t *)a->home.locator,strlen(a->home.locator)},seal};
}
static int same_record(adoption_t *a,const pxa_install_record_t *other) {
    uint8_t wire[PXA_INSTALL_RECORD_BYTES];
    return pxa_install_record_encode(&a->receipt.transfer.source,a->wire)==PXA_STATUS_OK &&
        pxa_install_record_encode(other,wire)==PXA_STATUS_OK && !memcmp(wire,a->wire,sizeof(wire));
}
static pxa_status_t dispose(adoption_t *a,pxa_status_t status) {
    if(!a)return status;
    if(status!=PXA_STATUS_OK)ESP_LOGW("PxaAdopt","Legacy adoption stopped at %s: app=%s status=%ld",
        a->stage?a->stage:"open",a->home.locator[0]?a->home.locator+65:"?",(long)status);
    pxa_status_t closed=pxa_esp_app_storage_close(&a->storage);
    if(a->storage) {pending_close=a;return closed;}
    const int descriptors[]={a->data_fd,a->home_fd,a->media_root,a->root};
    for(unsigned i=0;i<4;++i)if(descriptors[i]>=0 && close(descriptors[i]) && status==PXA_STATUS_OK)status=error();
    a->data_fd=a->home_fd=a->media_root=a->root=-1;
    if(a->media_epoch) {pxa_esp_volumes_release(a->media_epoch);a->media_epoch=0;}
    if(a->plan.reserved)pxa_esp_storage_release_plan(&a->plan);
    closed=pxa_esp_installations_close(&a->owner);
    if(a->owner) {pending_close=a;return closed;}
    if(status==PXA_STATUS_OK)status=closed;
    if(pending_close==a)pending_close=NULL;
    heap_caps_free(a->encoded);heap_caps_free(a->manifest_workspace);heap_caps_free(a->receipt_workspace);heap_caps_free(a);
    return status;
}
static adoption_t *allocate(uint64_t epoch,const char *locator,const pxa_esp_app_transfer_control_t *control,pxa_status_t *status) {
    *status=PXA_STATUS_RESOURCE_LIMIT;
    adoption_t *a=heap_caps_aligned_calloc(8,1,sizeof(*a),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!a)return NULL;
    a->root=a->home_fd=a->data_fd=a->media_root=-1;a->control=control;a->source.app_epoch=epoch;
    *status=pxa_esp_installations_open(epoch,&a->owner);if(*status!=PXA_STATUS_OK)return a;
    if(!(a->owner->volume.capabilities&PXA_VOLUME_PRIVATE_NATIVE)) {*status=PXA_STATUS_UNSUPPORTED;return a;}
    *status=pxa_esp_storage_volume_path(epoch,a->mount,sizeof(a->mount));if(*status!=PXA_STATUS_OK)return a;
    a->root=open(a->mount,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(a->root<0) {*status=error();return a;}
    a->receipt_workspace=heap_caps_aligned_calloc(8,1,pxa_esp_transfer_receipt_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!a->receipt_workspace) {*status=PXA_STATUS_RESOURCE_LIMIT;return a;}
    if(locator) {
        size_t n=strnlen(locator,sizeof(a->home.locator));
        if(n>=sizeof(a->home.locator) || n<66 || locator[64]!='/') {*status=PXA_STATUS_INVALID_ARGUMENT;return a;}
        memcpy(a->home.locator,locator,n+1);
        n=(size_t)snprintf(a->home.home,sizeof(a->home.home),"%s/apps/%s",a->owner->root,locator);
        if(n>=sizeof(a->home.home)) {*status=PXA_STATUS_LIMIT_EXCEEDED;return a;}
    }
    *status=PXA_STATUS_OK;return a;
}
static pxa_status_t bind_media(adoption_t *a) {
    if(!(a->receipt.transfer.source.flags&PXA_INSTALL_RECORD_HAS_MEDIA))return PXA_STATUS_OK;
    pxa_volume_info_t volume;pxa_status_t status=pxa_esp_storage_find_volume(&a->receipt.transfer.source.media_volume,&volume);
    if(status!=PXA_STATUS_OK)return PXA_STATUS_UNAVAILABLE;
    if(a->source.media_epoch && a->source.media_epoch!=volume.mount_epoch)return PXA_STATUS_BAD_STATE;
    status=pxa_esp_volumes_retain(volume.mount_epoch);if(status!=PXA_STATUS_OK)return status;
    a->media_epoch=a->source.media_epoch=volume.mount_epoch;
    status=pxa_esp_storage_volume_path(a->media_epoch,a->media_mount,sizeof(a->media_mount));
    if(status==PXA_STATUS_OK && (a->media_root=open(a->media_mount,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW))<0)status=error();
    return status;
}
/* The result owns only the exact bounded manifest, using PSRAM. File contents
 * are authenticated by the existing installer, without extracting/copying. */
static pxa_status_t inspect(adoption_t *a,int package,const char *path) {
    int file=openat(package,"manifest.pxm",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(file<0)return error();
    struct stat before,after;pxa_status_t status=PXA_STATUS_OK;
    if(fstat(file,&before))status=error();
    else if(!S_ISREG(before.st_mode) || before.st_nlink>1 || before.st_size<=0 ||
        (uint64_t)before.st_size>PXA_ESP_SIGNED_MANIFEST_MAX_BYTES)status=PXA_STATUS_DENIED;
    size_t size=status==PXA_STATUS_OK?(size_t)before.st_size:0,offset=0;
    if(status==PXA_STATUS_OK && !a->encoded) {
        a->encoded=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!a->encoded)status=PXA_STATUS_RESOURCE_LIMIT;
        else a->result.encoded_capacity=size;
    } else if(status==PXA_STATUS_OK && a->result.encoded_capacity!=size)status=PXA_STATUS_DENIED;
    while(status==PXA_STATUS_OK && offset<size) {
        status=available(a);if(status!=PXA_STATUS_OK)break;
        size_t amount=size-offset;if(amount>4096)amount=4096;
        ssize_t n=read(file,a->encoded+offset,amount);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) {status=PXA_STATUS_IO_ERROR;break;}offset+=(size_t)n;
    }
    if(status==PXA_STATUS_OK && (fstat(file,&after) || before.st_dev!=after.st_dev || before.st_ino!=after.st_ino ||
        before.st_size!=after.st_size || before.st_mtime!=after.st_mtime))status=PXA_STATUS_DENIED;
    if(close(file) && status==PXA_STATUS_OK)status=error();
    if(status!=PXA_STATUS_OK)return status;
    if(!a->manifest_workspace) {
        pxa_package_limits_t limits;
        status=pxa_package_manifest_measure((pxa_bytes_t){a->encoded,size},&a->owner->config.installer.limits,&limits);
        if(status!=PXA_STATUS_OK)return status;
        a->result.manifest_workspace_size=pxa_package_manifest_workspace_size(&limits);
        if(!a->result.manifest_workspace_size)return PXA_STATUS_DENIED;
        a->manifest_workspace=heap_caps_aligned_calloc(8,1,a->result.manifest_workspace_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!a->manifest_workspace)return PXA_STATUS_RESOURCE_LIMIT;
    }
    a->result.struct_size=sizeof(a->result);a->result.manifest=&a->manifest;
    a->result.encoded=a->encoded;a->result.manifest_workspace=a->manifest_workspace;
    a->result.root=a->result_root;a->result.root_capacity=sizeof(a->result_root);
    status=pxa_esp_installations_inspect(a->owner,path,&a->result);
    if(status==PXA_STATUS_OK && (a->manifest->package_kind==PXA_PACKAGE_KIND_RESOURCES || a->manifest->resource_manifest_sha256))
        status=PXA_STATUS_UNSUPPORTED;
    if(status==PXA_STATUS_OK && !a->source.media_epoch) {
        for(uint16_t i=0;i<a->manifest->component_count;++i) {
            const pxa_package_component_t *component=&a->manifest->components[i];
            for(uint16_t j=0;j<component->service_count;++j) {
                const pxa_package_service_requirement_t *requirement=&component->services[j];
                if(requirement->service==PXA_FS_SERVICE_ID && (requirement->required_features&
                    (PXA_FS_FEATURE_APP_MEDIA_ROOT|PXA_FS_FEATURE_APP_MEDIA_PATH)))return PXA_STATUS_UNAVAILABLE;
            }
        }
    }
    return status;
}
static pxa_status_t manifest_matches(adoption_t *a) {
    pxa_install_record_t *r=&a->receipt.transfer.source;uint8_t digest[32];
    pxa_package_publisher_lineage_t lineage;
    pxa_status_t status=pxa_package_publisher_lineage_parse(a->manifest,&lineage);
    if(status==PXA_STATUS_OK)status=pxa_openssl_sha256(a->manifest->encoded.data,a->manifest->encoded.size,digest);
    if(status==PXA_STATUS_OK && (memcmp(digest,r->manifest_sha256,32) || !lineage.root_key_id ||
        memcmp(lineage.root_key_id,r->publisher_root,32) || strlen(r->app_id)!=a->manifest->app_id.size ||
        memcmp(r->app_id,a->manifest->app_id.data,a->manifest->app_id.size) ||
        r->release_sequence!=(a->manifest->has_release_sequence?a->manifest->release_sequence:0)))status=PXA_STATUS_BAD_STATE;
    return status;
}
static pxa_status_t locate_home(adoption_t *a,uint8_t create) {
    int n=snprintf(a->scratch,sizeof(a->scratch),"PXA/apps/%s",a->home.locator);
    if(n<0 || (size_t)n>=sizeof(a->scratch))return PXA_STATUS_LIMIT_EXCEEDED;
    a->home_fd=walk(a->root,a->scratch,create);
    if(a->home_fd<0)return error();
    a->journal=(pxa_esp_transfer_receipt_config_t){a->home_fd,a->owner->volume.mount_epoch,PXA_ESP_TRANSFER_TARGET,a->home.locator};
    return PXA_STATUS_OK;
}
static pxa_status_t absent_controls(int root) {
    const char *names[]={".pxa-policy",".pxa-directory-grants"};
    for(unsigned i=0;i<2;++i) {
        struct stat metadata;
        if(!fstatat(root,names[i],&metadata,AT_SYMLINK_NOFOLLOW))return PXA_STATUS_DENIED;
        if(errno!=ENOENT)return error();
    }
    return PXA_STATUS_OK;
}
static pxa_status_t preflight(adoption_t *a,uint8_t snapshot) {
    a->stage="preflight-code";
    pxa_status_t status=available(a);if(status!=PXA_STATUS_OK)return status;
    int package=walk(a->root,a->source.package,0);if(package<0)return error();
    int n=snprintf(a->package,sizeof(a->package),"%s/%s",a->mount,a->source.package);
    status=n<0 || (size_t)n>=sizeof(a->package)?PXA_STATUS_LIMIT_EXCEEDED:inspect(a,package,a->package);
    if(close(package) && status==PXA_STATUS_OK)status=error();
    if(status!=PXA_STATUS_OK)return status;
    a->stage="preflight-private";
    int private_root=walk(a->root,a->source.private_root,0);
    if(private_root<0 && errno!=ENOENT)return error();
    uint8_t present=private_root>=0;
    if(!snapshot && present!=a->source.private_present)status=PXA_STATUS_BAD_STATE;
    if(snapshot)a->source.private_present=present;
    if(status==PXA_STATUS_OK && present)status=absent_controls(private_root);
    if(private_root>=0 && close(private_root) && status==PXA_STATUS_OK)status=error();
    if(status!=PXA_STATUS_OK)return status;
    if(present) {
        int count=snprintf(a->scratch,sizeof(a->scratch),"%s/%s",a->mount,a->source.private_root);
        if(count<0 || (size_t)count>=sizeof(a->scratch))return PXA_STATUS_LIMIT_EXCEEDED;
        void *workspace=heap_caps_aligned_calloc(8,1,pxa_esp_private_copy_workspace_size(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!workspace)return PXA_STATUS_RESOURCE_LIMIT;
        const pxa_esp_private_copy_config_t config={a->owner->volume.allocation_unit,UINT64_MAX,UINT64_MAX,UINT64_MAX,a,check_copy};
        pxa_esp_private_copy_info_t info;
        status=pxa_esp_private_copy_measure_install(a->scratch,&config,workspace,pxa_esp_private_copy_workspace_size(),&info);
        heap_caps_free(workspace);if(status!=PXA_STATUS_OK)return status;
    }
    a->stage="preflight-home";status=locate_home(a,0);
    if(status==PXA_STATUS_OK) {
        /* A partial very first journal may leave only unpublished .pending
         * files. No code/data can have moved before a durable first slot.
         * Fresh signed-source approval may repair just that empty intent;
         * committed/corrupt named slots, package or data are never erased. */
        int copy=dup(a->home_fd);DIR *entries=copy<0?NULL:fdopendir(copy);
        if(!entries) {if(copy>=0)close(copy);return error();}
        for(;;) {
            errno=0;struct dirent *entry=readdir(entries);
            if(!entry) {if(errno)status=error();break;}
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
            if(strcmp(entry->d_name,PXA_ESP_TRANSFER_SLOT_0 ".pending") &&
                strcmp(entry->d_name,PXA_ESP_TRANSFER_SLOT_1 ".pending")) {status=PXA_STATUS_BAD_STATE;break;}
            struct stat st;
            if(fstatat(a->home_fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)) {status=error();break;}
            if(!S_ISREG(st.st_mode) || st.st_nlink>1 || st.st_size<0 ||
                (uint64_t)st.st_size>16+PXA_INSTALL_TRANSFER_BYTES+PXA_ESP_TRANSFER_CONTEXT_BYTES+32) {status=PXA_STATUS_DENIED;break;}
            uint64_t allocation;
            status=pxa_install_file_allocation(&a->owner->volume,(uint64_t)st.st_size,&allocation);
            if(status!=PXA_STATUS_OK)break;
            a->repair_credit+=allocation;
        }
        if(closedir(entries) && status==PXA_STATUS_OK)status=error();
        if(close(a->home_fd) && status==PXA_STATUS_OK)status=error();
        a->home_fd=-1;
    } else if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;
    if(status!=PXA_STATUS_OK)return status;
    if(a->media_epoch) {
        a->stage="preflight-media";
        int from=walk(a->media_root,a->source.media,0);if(from<0)return error();
        if(close(from))return error();
        n=snprintf(a->scratch,sizeof(a->scratch),"PXA/media/%s",a->home.locator);
        if(n<0 || (size_t)n>=sizeof(a->scratch))return PXA_STATUS_LIMIT_EXCEEDED;
        if(strcmp(a->source.media,a->scratch)) {
            int to=walk(a->media_root,a->scratch,0);
            if(to>=0) {close(to);return PXA_STATUS_BAD_STATE;}
            if(errno!=ENOENT)return error();
        }
    }
    return status;
}
static pxa_status_t directory_credit(adoption_t *a,int root,const char *relative,uint64_t unit,uint64_t *credit) {
    (void)a;
    int fd=walk(root,relative,0);
    if(fd<0) {
        if(errno==ENOENT)return PXA_STATUS_OK;
        ESP_LOGW("PxaAdopt","Metadata directory unavailable: path=%s errno=%d",relative,errno);
        return error();
    }
    *credit+=unit;
    if(close(fd))return error();
    return PXA_STATUS_OK;
}
static pxa_status_t file_credit(adoption_t *a,int root,const char *path,uint64_t expected,uint64_t unit,uint64_t *credit) {
    const char *leaf;int fd=parent(a,root,path,&leaf,0);
    if(fd<0) {
        if(errno==ENOENT)return PXA_STATUS_OK;
        ESP_LOGW("PxaAdopt","Metadata parent unavailable: path=%s errno=%d",path,errno);
        return error();
    }
    struct stat st;pxa_status_t status=PXA_STATUS_OK;
    if(fstatat(fd,leaf,&st,AT_SYMLINK_NOFOLLOW)) {if(errno!=ENOENT)status=error();}
    else if(!S_ISREG(st.st_mode) || st.st_nlink>1 || st.st_size!=(off_t)expected)status=PXA_STATUS_DENIED;
    else *credit+=unit;
    if(close(fd) && status==PXA_STATUS_OK)status=error();
    if(status!=PXA_STATUS_OK)ESP_LOGW("PxaAdopt","Metadata credit failed: path=%s errno=%d",path,errno);
    return status;
}
static pxa_status_t admission(adoption_t *a,uint8_t reserve) {
    a->stage="admit-metadata";
    uint64_t receipt,record,policy,grants;
    pxa_status_t status=pxa_install_file_allocation(&a->owner->volume,
        16+PXA_INSTALL_TRANSFER_BYTES+a->receipt.context_size+32,&receipt);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&a->owner->volume,PXA_POSIX_INSTALL_RECORD_SEALED_BYTES,&record);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&a->owner->volume,32,&policy);
    if(status==PXA_STATUS_OK)status=pxa_install_file_allocation(&a->owner->volume,1264,&grants);
    if(status!=PXA_STATUS_OK)return status;
    /* No second code/private payload. Include future metadata replacement
     * peaks, each declared policy and both old/global grant source reads. */
    uint64_t bytes=3*receipt+3*record+(a->manifest->permission_count+1u)*policy+2*grants+
        UINT64_C(7)*a->owner->volume.allocation_unit+4096;
    uint64_t credit=a->repair_credit;pxa_status_t measured=PXA_STATUS_OK;
    const char *base[]={"PXA","PXA/apps"};
    a->stage="admit-directories";
    for(unsigned i=0;measured==PXA_STATUS_OK && i<2;++i)
        measured=directory_credit(a,a->root,base[i],a->owner->volume.allocation_unit,&credit);
    /* Existing allocations are already absent from free space. Account only
     * for future replacement peaks after a reboot, not another copy of all
     * journal-owned metadata on a nearly full source volume. */
    const char *tail[]={"","","/data","/data/0000000000000001","/data/0000000000000001/.pxa-policy"};
    char path[PXA_POSIX_APP_PATH_BYTES];
    for(unsigned i=0;measured==PXA_STATUS_OK && i<5;++i) {
        int n=i==0?snprintf(path,sizeof(path),"PXA/apps/%.64s",a->home.locator):
            snprintf(path,sizeof(path),"PXA/apps/%s%s",a->home.locator,tail[i]);
        if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
        measured=directory_credit(a,a->root,path,a->owner->volume.allocation_unit,&credit);
    }
    const char *files[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1,"install.0.pxi","install.1.pxi",
        "data/0000000000000001/.pxa-directory-grants"};
    a->stage="admit-files";
    for(unsigned i=0;measured==PXA_STATUS_OK && i<5;++i) {
        int n=snprintf(path,sizeof(path),"PXA/apps/%s/%s",a->home.locator,files[i]);
        if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
        const uint64_t logical=i<2?16+PXA_INSTALL_TRANSFER_BYTES+a->receipt.context_size+32:
            i<4?PXA_POSIX_INSTALL_RECORD_SEALED_BYTES:1264;
        measured=file_credit(a,a->root,path,logical,i<2?receipt:i<4?record:grants,&credit);
    }
    if(measured!=PXA_STATUS_OK)return measured;
    /* A retry resets policy entries before reimport. Their eventual space is
     * counted above; crediting them avoids charging it twice on recovery. */
    int n=snprintf(path,sizeof(path),"PXA/apps/%s/data/0000000000000001/.pxa-policy",a->home.locator);
    if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
    int policy_fd=walk(a->root,path,0);
    a->stage="admit-policy";
    if(policy_fd>=0) {
        int copy=dup(policy_fd);DIR *entries=copy<0?NULL:fdopendir(copy);
        if(!entries) {int saved=errno;if(copy>=0)close(copy);close(policy_fd);errno=saved;return error();}
        uint64_t known=0;
        for(;;) {
            errno=0;struct dirent *entry=readdir(entries);
            if(!entry) {if(errno)measured=error();break;}
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
            if(strlen(entry->d_name)!=64)continue;
            struct stat st;
            if(!fstatat(policy_fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW) &&
                S_ISREG(st.st_mode) && st.st_nlink<=1 && st.st_size==32 && known<a->manifest->permission_count) {++known;credit+=policy;}
        }
        if(closedir(entries) && measured==PXA_STATUS_OK)measured=error();
        if(close(policy_fd) && measured==PXA_STATUS_OK)measured=error();
        if(measured!=PXA_STATUS_OK)return measured;
    } else if(errno!=ENOENT)return error();
    if(credit>=bytes)return PXA_STATUS_DENIED;
    bytes-=credit;
    uint64_t media=0;
    if(a->media_epoch) {
        a->stage="admit-media";
        pxa_volume_info_t volume;status=pxa_esp_storage_volume(a->media_epoch,&volume);
        if(status!=PXA_STATUS_OK)return status;
        media=UINT64_C(3)*volume.allocation_unit+4096;
        uint64_t existing=0;
        measured=directory_credit(a,a->media_root,"PXA",volume.allocation_unit,&existing);
        if(measured==PXA_STATUS_OK)measured=directory_credit(a,a->media_root,"PXA/media",volume.allocation_unit,&existing);
        n=snprintf(path,sizeof(path),"PXA/media/%.64s",a->home.locator);
        if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
        if(measured==PXA_STATUS_OK)measured=directory_credit(a,a->media_root,path,volume.allocation_unit,&existing);
        if(measured!=PXA_STATUS_OK)return measured;
        media-=existing;
    }
    a->stage="admit-plan";
    const pxa_install_requirements_t requirements={.app_bytes=bytes,.media_bytes=media};
    status=pxa_esp_storage_plan(&requirements,a->owner->volume.mount_epoch,a->media_epoch,&a->plan);
    return status==PXA_STATUS_OK && reserve?pxa_esp_storage_reserve_plan(&a->plan):status;
}
static pxa_status_t persist(adoption_t *a,uint8_t phase) {
    uint8_t durable=0;uint64_t revision=a->receipt.transfer.revision;
    pxa_status_t status;
    if(!revision) {a->receipt.transfer.revision=1;a->receipt.transfer.phase=PXA_INSTALL_TRANSFER_COPYING;}
    else {
        status=pxa_install_transfer_advance(&a->receipt.transfer,phase,&a->receipt.transfer);
        if(status!=PXA_STATUS_OK)return status;
    }
    return pxa_esp_transfer_receipt_store(&a->journal,revision,&a->receipt,a->receipt_workspace,
        pxa_esp_transfer_receipt_workspace_size(),&durable);
}
/* A single journal owns both paths. Exactly one existing directory is valid;
 * neither means data loss, both means conflict. Re-establish both durability
 * barriers even when replay observes an already completed rename. */
static pxa_status_t move_directory(adoption_t *a,int root,const char *source,int destination,const char *name,uint8_t was_present) {
    const char *leaf;int from=parent(a,root,source,&leaf,0);
    if(from<0 && (errno!=ENOENT || was_present))return error();
    struct stat old,next;pxa_status_t status=PXA_STATUS_OK;
    int have_old=from>=0 && !fstatat(from,leaf,&old,AT_SYMLINK_NOFOLLOW);int old_error=from>=0?errno:ENOENT;
    int have_next=!fstatat(destination,name,&next,AT_SYMLINK_NOFOLLOW);int next_error=errno;
    if((!have_old && old_error!=ENOENT) || (!have_next && next_error!=ENOENT))status=PXA_STATUS_IO_ERROR;
    else if((have_old && !S_ISDIR(old.st_mode)) || (have_next && !S_ISDIR(next.st_mode)) ||
        (have_old && have_next) || (was_present && !have_old && !have_next) || (!was_present && have_old))status=PXA_STATUS_DENIED;
    else if(have_old) {if(renameat(from,leaf,destination,name))status=error();}
    else if(!have_next && mkdirat(destination,name,0700))status=error();
    if(status==PXA_STATUS_OK && ((from>=0 && fsync(from)) || fsync(destination)))status=error();
    if(from>=0 && close(from) && status==PXA_STATUS_OK)status=error();
    return status;
}
static pxa_status_t require_retired_private(adoption_t *a) {
    const char *leaf;int old=parent(a,a->root,a->source.private_root,&leaf,0);
    if(old<0 && errno!=ENOENT)return error();
    pxa_status_t status=PXA_STATUS_OK;struct stat st;
    if(old>=0) {
        if(!fstatat(old,leaf,&st,AT_SYMLINK_NOFOLLOW))status=PXA_STATUS_DENIED;
        else if(errno!=ENOENT)status=error();
        if(close(old) && status==PXA_STATUS_OK)status=error();
    }
    if(status!=PXA_STATUS_OK)return status;
    int data=directory(a->home_fd,"data",0);if(data<0)return error();
    int private_root=directory(data,"0000000000000001",0);status=private_root<0?error():PXA_STATUS_OK;
    if(private_root>=0 && close(private_root) && status==PXA_STATUS_OK)status=error();
    if(close(data) && status==PXA_STATUS_OK)status=error();
    return status;
}
static pxa_status_t authority(adoption_t *a,uint8_t preflight_only) {
    a->stage=preflight_only?"preflight-authority":"import-authority";
    int grants=walk(a->root,CONFIG_PXA_STATE_ROOT "/grants",0);
    if(grants<0 && errno!=ENOENT)return error();
    const pxa_permission_store_t source={.struct_size=sizeof(source),.load=pxa_esp_permission_store_peek};
    pxa_bytes_t identity={(const uint8_t *)a->identity,strlen(a->identity)};
    pxa_status_t status;
    if(preflight_only)status=pxa_esp_private_authority_validate_legacy(identity,&source,a->manifest->permissions,
        a->manifest->permission_count,grants,a->identity);
    else {
        a->home.record=a->receipt.transfer.source;
        status=pxa_esp_app_storage_create(&a->home,a->source.app_epoch,pxa_esp_installations_private_capacity(),&a->plan,&a->storage);
        const char *root=NULL;
        if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(a->storage,&root);
        /* Only the authenticated READY journal permits resetting its partial
         * destination controls. Existing private data is never reset. */
        const char *controls[]={".pxa-policy",".pxa-directory-grants"};
        for(unsigned i=0;status==PXA_STATUS_OK && i<2;++i) {
            int n=snprintf(a->scratch,sizeof(a->scratch),"%s/%s",root,controls[i]);
            if(n<0 || (size_t)n>=sizeof(a->scratch)) {status=PXA_STATUS_LIMIT_EXCEEDED;break;}
            if(!i) {status=pxa_posix_fs_remove_tree(a->scratch);if(status==PXA_STATUS_NOT_FOUND)status=PXA_STATUS_OK;}
            else {
                int directory_fd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                if(directory_fd<0)status=error();
                else {
                    if(unlinkat(directory_fd,controls[i],0) && errno!=ENOENT)status=error();
                    if(status==PXA_STATUS_OK && fsync(directory_fd))status=error();
                    if(close(directory_fd) && status==PXA_STATUS_OK)status=error();
                }
            }
        }
        if(status==PXA_STATUS_OK)status=pxa_esp_private_authority_import_legacy(a->storage,identity,&source,
            a->manifest->permissions,a->manifest->permission_count,grants,a->identity);
        pxa_status_t closed=pxa_esp_app_storage_close(&a->storage);if(status==PXA_STATUS_OK)status=closed;
    }
    if(grants>=0 && close(grants) && status==PXA_STATUS_OK)status=error();
    return status;
}
static pxa_status_t collect_legacy_files(adoption_t *a) {
    pxa_status_t status=PXA_STATUS_OK;int grants=walk(a->root,CONFIG_PXA_STATE_ROOT "/grants",0);
    if(grants>=0) {
        if(unlinkat(grants,a->identity,0) && errno!=ENOENT)status=error();
        if(fsync(grants) && status==PXA_STATUS_OK)status=error();
        if(close(grants) && status==PXA_STATUS_OK)status=error();
    } else if(errno!=ENOENT)status=error();
    int owners=walk(a->root,CONFIG_PXA_STATE_ROOT "/owners",0);
    if(owners>=0) {
        char name[130];snprintf(name,sizeof(name),"%s",a->identity);name[64]='~';
        int fd=openat(owners,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd>=0) {
            uint8_t owner[40];struct stat st;
            ssize_t size=read(fd,owner,sizeof(owner));
            int valid=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==sizeof(owner) &&
                size==sizeof(owner) && !memcmp(owner,"PXAO\1\0\0\0",8) &&
                !memcmp(owner+8,a->receipt.transfer.source.publisher_root,32);
            if(close(fd) && status==PXA_STATUS_OK)status=error();
            if(valid) {if(unlinkat(owners,name,0) && errno!=ENOENT && status==PXA_STATUS_OK)status=error();}
            else if(status==PXA_STATUS_OK)status=PXA_STATUS_DENIED;
        } else if(errno!=ENOENT && status==PXA_STATUS_OK)status=error();
        if(fsync(owners) && status==PXA_STATUS_OK)status=error();
        if(close(owners) && status==PXA_STATUS_OK)status=error();
    } else if(errno!=ENOENT && status==PXA_STATUS_OK)status=error();
    if(status==PXA_STATUS_OK) {
        int packages=walk(a->root,CONFIG_PXA_STATE_ROOT "/" CONFIG_PXA_PACKAGES_DIR,0);
        if(packages>=0) {
            snprintf(a->scratch,sizeof(a->scratch),"%s.lock",a->identity);a->scratch[64]='~';
            int fd=openat(packages,a->scratch,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if(fd>=0) {
                struct stat st;int valid=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==0;
                if(close(fd))status=error();
                if(valid) {if(unlinkat(packages,a->scratch,0) && errno!=ENOENT && status==PXA_STATUS_OK)status=error();}
                else if(status==PXA_STATUS_OK)status=PXA_STATUS_DENIED;
            } else if(errno!=ENOENT)status=error();
            if(fsync(packages) && status==PXA_STATUS_OK)status=error();
            if(close(packages) && status==PXA_STATUS_OK)status=error();
        } else if(errno!=ENOENT)status=error();
    }
    return status;
}
static void collect_legacy_authority(adoption_t *a) {
    /* Local code, data and authenticated authority are already published and
     * mirrored. Old Flash/NVS collection must never gate their activation. */
    pxa_status_t status=pxa_esp_permission_store_clear_app(
        (pxa_bytes_t){(const uint8_t *)a->identity,strlen(a->identity)},
        a->manifest->permissions,a->manifest->permission_count);
    pxa_status_t files=collect_legacy_files(a);
    if(status==PXA_STATUS_OK)status=files;
    if(status!=PXA_STATUS_OK)ESP_LOGW("PxaAdopt","Legacy metadata collection deferred: app=%s status=%ld",
        a->receipt.transfer.source.app_id,(long)status);
}
static pxa_status_t run(adoption_t *a,pxa_posix_app_home_t *home) {
    pxa_status_t status=available(a);if(status!=PXA_STATUS_OK)return status;
    pxa_install_transfer_t *t=&a->receipt.transfer;
    const char *leaf;int from=parent(a,a->root,a->source.package,&leaf,0);
    if(from<0)return error();
    int package=directory(from,leaf,0);int saved=errno;close(from);errno=saved;
    if(package<0 && errno!=ENOENT)return error();
    if(package>=0) {
        if(t->phase>=PXA_INSTALL_TRANSFER_READY) {close(package);return PXA_STATUS_DENIED;}
        int n=snprintf(a->package,sizeof(a->package),"%s/%s",a->mount,a->source.package);
        if(n<0 || (size_t)n>=sizeof(a->package)) {close(package);return PXA_STATUS_LIMIT_EXCEEDED;}
    } else {
        package=directory(a->home_fd,"package",0);if(package<0)return error();
        status=pxa_posix_app_home_path(&a->home,PXA_POSIX_APP_HOME_PACKAGE,a->package,sizeof(a->package));
    }
    if(status==PXA_STATUS_OK)status=inspect(a,package,a->package);
    if(close(package) && status==PXA_STATUS_OK)status=error();
    if(status==PXA_STATUS_OK)status=manifest_matches(a);
    if(status!=PXA_STATUS_OK)return status;
    a->home.record=t->source;
    /* A previously published record is a stronger boundary than a replayed
     * old READY receipt. Never reset its private authority and import the
     * legacy grants again after publication. */
    pxa_posix_install_record_auth_t existing_auth=auth(a);pxa_install_record_t existing;
    status=pxa_posix_install_record_load_with_auth(a->home_fd,&a->owner->volume.id,&existing_auth,&existing);
    if(status==PXA_STATUS_OK) {
        if(t->phase<PXA_INSTALL_TRANSFER_SURRENDERED || !same_record(a,&existing))return PXA_STATUS_DENIED;
    } else if(status!=PXA_STATUS_NOT_FOUND)return status;
    if(!a->plan.reserved) {
        status=admission(a,1);if(status!=PXA_STATUS_OK)return status;
    }
    if(t->phase==PXA_INSTALL_TRANSFER_COPYING) {
        status=move_directory(a,a->root,a->source.package,a->home_fd,"package",1);
        if(status==PXA_STATUS_OK)status=checkpoint(a,2);
        if(status!=PXA_STATUS_OK)return status;
        a->data_fd=directory(a->home_fd,"data",1);if(a->data_fd<0)return error();
        status=move_directory(a,a->root,a->source.private_root,a->data_fd,"0000000000000001",a->source.private_present);
        if(status==PXA_STATUS_OK)status=checkpoint(a,3);
        if(status!=PXA_STATUS_OK)return status;
        status=persist(a,PXA_INSTALL_TRANSFER_READY);
        if(status==PXA_STATUS_OK)status=checkpoint(a,4);
    }
    if(status==PXA_STATUS_OK && t->phase==PXA_INSTALL_TRANSFER_READY) {
        status=require_retired_private(a);if(status!=PXA_STATUS_OK)return status;
        status=authority(a,0);
        if(status==PXA_STATUS_OK)status=checkpoint(a,5);
        if(status==PXA_STATUS_OK && a->media_epoch) {
            int n=snprintf(a->scratch,sizeof(a->scratch),"PXA/media/%s",a->home.locator);
            if(n<0 || (size_t)n>=sizeof(a->scratch))return PXA_STATUS_LIMIT_EXCEEDED;
            if(strcmp(a->source.media,a->scratch)) {
                const char *name;int target=parent(a,a->media_root,a->scratch,&name,1);
                if(target<0)return error();
                /* parent() reuses scratch, so the leaf must be derived from
                 * the immutable canonical locator instead of that buffer. */
                status=move_directory(a,a->media_root,a->source.media,target,strchr(a->home.locator,'/')+1,1);
                if(close(target) && status==PXA_STATUS_OK)status=error();
            }
        }
        if(status==PXA_STATUS_OK)status=checkpoint(a,6);
        if(status==PXA_STATUS_OK && (t->source.flags&PXA_INSTALL_RECORD_HAS_MEDIA))
            status=pxa_esp_installations_verify_media(a->owner,&t->source);
        if(status==PXA_STATUS_OK)status=persist(a,PXA_INSTALL_TRANSFER_SURRENDERED);
        if(status==PXA_STATUS_OK)status=checkpoint(a,7);
    }
    if(status!=PXA_STATUS_OK)return status;
    status=require_retired_private(a);if(status!=PXA_STATUS_OK)return status;
    /* Before publishing, reverify the only executable's exact signed tree.
     * A durable journal is recovery authority, never executable authority. */
    a->home.record=t->source;
    status=pxa_posix_app_home_path(&a->home,PXA_POSIX_APP_HOME_PACKAGE,a->package,sizeof(a->package));
    package=status==PXA_STATUS_OK?directory(a->home_fd,"package",0):-1;
    if(package<0)return status==PXA_STATUS_OK?error():status;
    status=inspect(a,package,a->package);if(close(package) && status==PXA_STATUS_OK)status=error();
    if(status==PXA_STATUS_OK)status=manifest_matches(a);
    if(status==PXA_STATUS_OK && (t->source.flags&PXA_INSTALL_RECORD_HAS_MEDIA))
        status=pxa_esp_installations_verify_media(a->owner,&t->source);
    if(status==PXA_STATUS_OK) {
        const char *root;
        status=pxa_esp_app_storage_create(&a->home,a->source.app_epoch,pxa_esp_installations_private_capacity(),&a->plan,&a->storage);
        if(status==PXA_STATUS_OK)status=pxa_esp_app_storage_existing_root(a->storage,&root);
        pxa_status_t closed=pxa_esp_app_storage_close(&a->storage);if(status==PXA_STATUS_OK)status=closed;
    }
    if(status!=PXA_STATUS_OK)return status;
    pxa_posix_install_record_auth_t record_auth=auth(a);pxa_install_record_t current;
    status=pxa_posix_install_record_load_with_auth(a->home_fd,&a->owner->volume.id,&record_auth,&current);
    if(status==PXA_STATUS_OK && !same_record(a,&current))return PXA_STATUS_DENIED;
    if(status==PXA_STATUS_NOT_FOUND)status=pxa_posix_install_record_commit_with_auth(a->home_fd,0,&t->source,&record_auth);
    if(status==PXA_STATUS_OK)status=checkpoint(a,8);
    if(status==PXA_STATUS_OK)status=pxa_posix_install_record_mirror(a->home_fd,&t->source,&record_auth);
    if(status==PXA_STATUS_OK)status=checkpoint(a,9);
    if(status==PXA_STATUS_OK && t->phase<PXA_INSTALL_TRANSFER_COMMITTED)status=persist(a,PXA_INSTALL_TRANSFER_COMMITTED);
    if(status==PXA_STATUS_OK)status=checkpoint(a,10);
    if(status!=PXA_STATUS_OK)return status;
    collect_legacy_authority(a);
    const char *slots[]={PXA_ESP_TRANSFER_SLOT_0,PXA_ESP_TRANSFER_SLOT_1,
        PXA_ESP_TRANSFER_SLOT_0 ".pending",PXA_ESP_TRANSFER_SLOT_1 ".pending"};
    for(unsigned i=0;i<4;++i)if(unlinkat(a->home_fd,slots[i],0) && errno!=ENOENT)return error();
    if(fsync(a->home_fd))return error();
    *home=a->home;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_legacy_adoption_prepare(pxa_esp_legacy_source_t *source,uint8_t enabled,
    pxa_install_record_t *record,pxa_install_plan_t *plan) {
    if(!source || !record || !plan || !valid_source(source) || enabled>1)return PXA_STATUS_INVALID_ARGUMENT;
    memset(record,0,sizeof(*record));memset(plan,0,sizeof(*plan));if(pending_close)return PXA_STATUS_BUSY;
    pxa_status_t status;adoption_t *a=allocate(source->app_epoch,NULL,NULL,&status);
    if(status!=PXA_STATUS_OK)return dispose(a,status);
    a->source=*source;
    a->stage="inspect-legacy-code";
    /* Read the source before deriving its canonical home. */
    int package=walk(a->root,source->package,0);
    if(package<0)return dispose(a,error());
    int n=snprintf(a->package,sizeof(a->package),"%s/%s",a->mount,source->package);
    status=n<0 || (size_t)n>=sizeof(a->package)?PXA_STATUS_LIMIT_EXCEEDED:inspect(a,package,a->package);
    if(close(package) && status==PXA_STATUS_OK)status=error();
    pxa_package_publisher_lineage_t lineage;
    if(status==PXA_STATUS_OK)status=pxa_package_publisher_lineage_parse(a->manifest,&lineage);
    pxa_install_record_t *r=&a->receipt.transfer.source;
    if(status==PXA_STATUS_OK) {
        if(!lineage.root_key_id)return dispose(a,PXA_STATUS_DENIED);
        r->generation=r->data_generation=1;r->flags=enabled?PXA_INSTALL_RECORD_ENABLED:0;r->app_volume=a->owner->volume.id;
        memcpy(r->publisher_root,lineage.root_key_id,32);memcpy(r->app_id,a->manifest->app_id.data,a->manifest->app_id.size);
        r->app_id[a->manifest->app_id.size]=0;r->release_sequence=a->manifest->has_release_sequence?a->manifest->release_sequence:0;
        esp_fill_random(r->install_id,16);
        status=pxa_openssl_sha256(a->manifest->encoded.data,a->manifest->encoded.size,r->manifest_sha256);
    }
    if(status==PXA_STATUS_OK && source->media_epoch) {
        pxa_volume_info_t media;status=pxa_esp_storage_volume(source->media_epoch,&media);
        if(status==PXA_STATUS_OK) {r->flags|=PXA_INSTALL_RECORD_HAS_MEDIA;r->media_volume=media.id;}
    }
    if(status==PXA_STATUS_OK)status=pxa_esp_installations_locator(r->publisher_root,r->app_id,a->home.locator,sizeof(a->home.locator));
    if(status==PXA_STATUS_OK) {
        n=snprintf(a->home.home,sizeof(a->home.home),"%s/apps/%s",a->owner->root,a->home.locator);
        if(n<0 || (size_t)n>=sizeof(a->home.home))status=PXA_STATUS_LIMIT_EXCEEDED;
        snprintf(a->identity,sizeof(a->identity),"%s",a->home.locator);a->identity[64]=':';
    }
    if(status==PXA_STATUS_OK)status=bind_media(a);
    if(status==PXA_STATUS_OK)status=preflight(a,1);
    if(status==PXA_STATUS_OK)status=authority(a,1);
    if(status==PXA_STATUS_OK)status=context_pack(a);
    if(status==PXA_STATUS_OK)status=admission(a,0);
    if(status==PXA_STATUS_OK) {*source=a->source;*record=*r;*plan=a->plan;}
    return dispose(a,status);
}
pxa_status_t pxa_esp_legacy_adoption_begin(const pxa_esp_legacy_source_t *source,const pxa_install_record_t *approved,
    const pxa_esp_app_transfer_control_t *control,pxa_posix_app_home_t *home) {
    if(!source || !approved || !home || !valid_source(source))return PXA_STATUS_INVALID_ARGUMENT;
    memset(home,0,sizeof(*home));if(pending_close)return PXA_STATUS_BUSY;
    if(!!source->media_epoch!=!!(approved->flags&PXA_INSTALL_RECORD_HAS_MEDIA))return PXA_STATUS_BAD_STATE;
    char locator[PXA_POSIX_APP_LOCATOR_BYTES];pxa_status_t status=pxa_esp_installations_locator(approved->publisher_root,approved->app_id,locator,sizeof(locator));
    if(status!=PXA_STATUS_OK)return status;
    adoption_t *a=allocate(source->app_epoch,locator,control,&status);if(status!=PXA_STATUS_OK)return dispose(a,status);
    a->source=*source;a->receipt.transfer.source=*approved;a->receipt.transfer.kind=PXA_INSTALL_TRANSFER_KIND_LEGACY_ADOPT;
    a->receipt.transfer.target_app_volume=a->owner->volume.id;a->receipt.transfer.target_media_volume=approved->media_volume;
    a->receipt.transfer.revision=1;a->receipt.transfer.phase=PXA_INSTALL_TRANSFER_COPYING;esp_fill_random(a->receipt.transfer.transfer_id,16);
    uint8_t wire[PXA_INSTALL_TRANSFER_BYTES];status=pxa_install_transfer_encode(&a->receipt.transfer,wire);
    if(status==PXA_STATUS_OK)status=bind_media(a);
    if(status==PXA_STATUS_OK)status=preflight(a,0);
    if(status==PXA_STATUS_OK)status=manifest_matches(a);
    snprintf(a->identity,sizeof(a->identity),"%s",locator);a->identity[64]=':';
    if(status==PXA_STATUS_OK)status=authority(a,1);
    if(status==PXA_STATUS_OK)status=context_pack(a);
    if(status==PXA_STATUS_OK)status=admission(a,1);
    if(status==PXA_STATUS_OK)status=locate_home(a,1);
    if(status==PXA_STATUS_OK && a->repair_credit) {
        const char *temporary[]={PXA_ESP_TRANSFER_SLOT_0 ".pending",PXA_ESP_TRANSFER_SLOT_1 ".pending"};
        for(unsigned i=0;i<2;++i)if(unlinkat(a->home_fd,temporary[i],0) && errno!=ENOENT) {status=error();break;}
        if(status==PXA_STATUS_OK && fsync(a->home_fd))status=error();
    }
    if(status==PXA_STATUS_OK) {a->receipt.transfer.revision=0;status=persist(a,PXA_INSTALL_TRANSFER_COPYING);}
    if(status==PXA_STATUS_OK)status=checkpoint(a,1);
    if(status==PXA_STATUS_OK)status=run(a,home);
    return dispose(a,status);
}
pxa_status_t pxa_esp_legacy_adoption_resume(uint64_t epoch,const char *locator,
    const pxa_esp_app_transfer_control_t *control,pxa_posix_app_home_t *home) {
    if(!home)return PXA_STATUS_INVALID_ARGUMENT;
    memset(home,0,sizeof(*home));
    if(pending_close) {pxa_status_t retried=dispose(pending_close,PXA_STATUS_OK);if(pending_close)return retried;}
    pxa_status_t status;adoption_t *a=allocate(epoch,locator,control,&status);
    if(status==PXA_STATUS_OK)status=locate_home(a,0);
    if(status==PXA_STATUS_OK)status=pxa_esp_transfer_receipt_load(&a->journal,a->receipt_workspace,
        pxa_esp_transfer_receipt_workspace_size(),&a->receipt);
    if(status==PXA_STATUS_OK && a->receipt.transfer.kind!=PXA_INSTALL_TRANSFER_KIND_LEGACY_ADOPT)status=PXA_STATUS_UNSUPPORTED;
    if(status==PXA_STATUS_OK && a->receipt.transfer.phase==PXA_INSTALL_TRANSFER_ABORTED)status=PXA_STATUS_DENIED;
    if(status==PXA_STATUS_OK)status=bind_media(a);
    if(status==PXA_STATUS_OK)status=context_unpack(a);
    if(status==PXA_STATUS_OK) {
        snprintf(a->identity,sizeof(a->identity),"%s",a->home.locator);a->identity[64]=':';
        /* Resuming the same bounded metadata peak never duplicates payload. */
        status=run(a,home);
    }
    return dispose(a,status);
}
