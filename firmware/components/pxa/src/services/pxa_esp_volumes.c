#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_volumes.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "pxa/posix/pxa_posix_volume.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "mbedtls/md.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    void *context;
    pxa_esp_storage_space_fn query_space;
    int (*sync_directory)(void *,const char *);
} volume_driver_t;
static void *g_workspace;
static pxa_volume_registry_t *g_registry;
static pxa_scoped_fs_mount_t g_mounts[PXA_ESP_STORAGE_MAX_VOLUMES];
static volume_driver_t g_drivers[PXA_ESP_STORAGE_MAX_VOLUMES];
static uint32_t g_references[PXA_ESP_STORAGE_MAX_VOLUMES];
typedef struct private_mount {
    struct private_mount *next;
    uint64_t epoch;
    uint8_t virtual_mount;
    char path[];
} private_mount_t;
/* Up to24 app sessions can retain home, private VFS and external media roots. */
#define MAX_PRIVATE_MOUNTS 72u
static private_mount_t *g_private_mounts;
static pthread_once_t g_lock_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock;

static void initialize_lock(void) {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock,&attributes);
    pthread_mutexattr_destroy(&attributes);
}
void pxa_esp_volumes_lock(void) { pthread_once(&g_lock_once,initialize_lock); pthread_mutex_lock(&g_lock); }
void pxa_esp_volumes_unlock(void) { pthread_mutex_unlock(&g_lock); }
pxa_volume_registry_t *pxa_esp_volumes_registry(void) { return g_registry; }
const pxa_scoped_fs_mount_t *pxa_esp_volumes_mounts(void) { return g_mounts; }

static int slot_for_epoch(uint64_t epoch) {
    if(epoch) for(unsigned i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i)
        if(g_mounts[i].mount_epoch==epoch)return (int)i;
    return -1;
}
static int slot_for_path(const char *path) {
    int found=-1;size_t longest=0;
    for(unsigned i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        const char *mount=g_mounts[i].host_path;
        if(!mount)continue;
        size_t length=strlen(mount);
        if(length>longest && !strncmp(path,mount,length) &&
            (!path[length] || path[length]=='/')) {found=(int)i;longest=length;}
    }
    return found;
}
static private_mount_t *private_for_path(const char *path) {
    private_mount_t *found=NULL;size_t longest=0;
    for(private_mount_t *mount=g_private_mounts;mount;mount=mount->next) {
        size_t length=strlen(mount->path);
        if(length>longest && !strncmp(path,mount->path,length) &&
            (!path[length] || path[length]=='/')) {found=mount;longest=length;}
    }
    return found;
}
pxa_status_t pxa_esp_storage_resolve_volume(const char *path,pxa_volume_info_t *output) {
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    memset(output,0,sizeof(*output));
    if(!path || path[0]!='/' || !pxa_fs_scoped_path_is_valid(
        (pxa_bytes_t){(const uint8_t *)path+1,strlen(path+1)}))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_esp_volumes_lock();int slot=slot_for_path(path);private_mount_t *private=private_for_path(path);
    uint64_t epoch=private?private->epoch:slot>=0?g_mounts[slot].mount_epoch:0;
    pxa_status_t status=epoch && g_registry?pxa_volume_get(g_registry,epoch,output):PXA_STATUS_NOT_FOUND;
    if(status==PXA_STATUS_OK && (output->state!=PXA_VOLUME_READY || output->ambiguous))status=PXA_STATUS_UNAVAILABLE;
    if(status!=PXA_STATUS_OK)memset(output,0,sizeof(*output));
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_usage(uint64_t *total,uint64_t *free_bytes,size_t *count) {
    if(!total || !free_bytes || !count)return PXA_STATUS_INVALID_ARGUMENT;
    *total=*free_bytes=0;*count=0;
    uint64_t sum=0,free=0;size_t volumes=0;pxa_status_t status=PXA_STATUS_OK;pxa_volume_info_t volume;
    pxa_esp_volumes_lock();
    for(unsigned i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
        uint64_t epoch=g_mounts[i].mount_epoch;
        if(!epoch || !g_registry || pxa_volume_get(g_registry,epoch,&volume)!=PXA_STATUS_OK ||
            volume.state!=PXA_VOLUME_READY || volume.ambiguous)continue;
        if((volume.space_known&(PXA_VOLUME_TOTAL_KNOWN|PXA_VOLUME_FREE_KNOWN))!=
            (PXA_VOLUME_TOTAL_KNOWN|PXA_VOLUME_FREE_KNOWN)) {status=PXA_STATUS_UNAVAILABLE;break;}
        if(volume.free_bytes>volume.total_bytes || sum>UINT64_MAX-volume.total_bytes || free>UINT64_MAX-volume.free_bytes) {
            status=PXA_STATUS_RESOURCE_LIMIT;break;
        }
        sum+=volume.total_bytes;free+=volume.free_bytes;++volumes;
    }
    pxa_esp_volumes_unlock();
    if(status==PXA_STATUS_OK && !volumes)status=PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK) {*total=sum;*free_bytes=free;*count=volumes;}
    return status;
}
static uint64_t directory_epoch(void *context,const char *path) {
    pxa_volume_info_t info;uint64_t epoch=0;int slot;
    (void)context;
    pxa_esp_volumes_lock();slot=slot_for_path(path);
    private_mount_t *private=private_for_path(path);
    if(private)epoch=private->epoch;
    else if(slot>=0)epoch=g_mounts[slot].mount_epoch;
    if(epoch) {
        if(pxa_volume_get(g_registry,epoch,&info)!=PXA_STATUS_OK ||
            info.state!=PXA_VOLUME_READY || info.ambiguous)epoch=UINT64_MAX;
    }
    pxa_esp_volumes_unlock();return epoch;
}
static int sync_directory(void *context,const char *path,uint64_t epoch) {
    int slot,result=0;pxa_volume_info_t info;
    (void)context;
    pxa_esp_volumes_lock();slot=slot_for_path(path);
    private_mount_t *private=private_for_path(path);
    if(private && private->virtual_mount) {
        if(private->epoch!=epoch || pxa_volume_get(g_registry,epoch,&info)!=PXA_STATUS_OK ||
            info.state!=PXA_VOLUME_READY || info.ambiguous) {errno=EIO;result=-1;}
        /* LittleFS committed metadata through the image's fsync barrier.
         * The physical driver's path callback cannot accept a VFS alias. */
    } else if(slot>=0) {
        if(g_mounts[slot].mount_epoch!=epoch ||
            pxa_volume_get(g_registry,epoch,&info)!=PXA_STATUS_OK ||
            info.state!=PXA_VOLUME_READY || info.ambiguous) {errno=EIO;result=-1;}
        else if(g_drivers[slot].sync_directory)
            result=g_drivers[slot].sync_directory(g_drivers[slot].context,path);
    } else if(epoch) {errno=EIO;result=-1;}
    pxa_esp_volumes_unlock();return result;
}
pxa_status_t pxa_esp_volumes_initialize(void) {
    pxa_status_t status=PXA_STATUS_OK;
    pxa_esp_posix_directory_hooks_t hooks={NULL,directory_epoch,sync_directory};
    pxa_esp_volumes_lock();
    if(!g_registry) {
        size_t bytes=pxa_volume_registry_workspace_size(PXA_ESP_STORAGE_MAX_VOLUMES);
        if(!g_workspace)g_workspace=heap_caps_calloc(1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!g_workspace)g_workspace=heap_caps_calloc(1,bytes,MALLOC_CAP_8BIT);
        uint64_t seed=(((uint64_t)esp_random()<<32)|esp_random())&(UINT64_MAX>>1);
        if(!g_workspace)status=PXA_STATUS_RESOURCE_LIMIT;
        else if(pxa_esp_posix_set_directory_hooks(&hooks)!=0)status=PXA_STATUS_BUSY;
        else status=pxa_volume_registry_init(g_workspace,bytes,
            PXA_ESP_STORAGE_MAX_VOLUMES,seed?seed:1,&g_registry);
    }
    pxa_esp_volumes_unlock();return status;
}
static int sha256(const void *bytes,size_t count,uint8_t output[32]) {
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md && mbedtls_md(md,bytes,count,output)==0;
}
static pxa_status_t local_identity(const pxa_esp_storage_volume_config_t *config,
    pxa_volume_id_t *id,uint8_t *strength) {
    char path[PXA_ESP_STORAGE_PATH_BYTES+8];int directory;
    pxa_status_t status;pxa_volume_id_t proposed;
    int n=snprintf(path,sizeof(path),"%s/PXA",config->mount_path);
    if(n<0 || (size_t)n>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
    if(config->capabilities&PXA_VOLUME_WRITE) {
        if(mkdir(path,0700)!=0 && errno!=EEXIST)return PXA_STATUS_IO_ERROR;
    }
    directory=pxa_esp_open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(directory<0)status=errno==ENOENT?PXA_STATUS_NOT_FOUND:PXA_STATUS_IO_ERROR;
    else {
        if(config->capabilities&PXA_VOLUME_WRITE) {
            do {for(unsigned i=0;i<16;i+=4) {uint32_t random=esp_random();memcpy(proposed.bytes+i,&random,4);}}
            while(!memcmp(proposed.bytes,(uint8_t[16]){0},16));
            status=pxa_posix_volume_identity_initialize(directory,&proposed,id);
        } else status=pxa_posix_volume_identity_load(directory,id);
        if(pxa_esp_close(directory)!=0 && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    }
    if(status==PXA_STATUS_OK) {*strength=PXA_VOLUME_ID_FILESYSTEM;return status;}
    if(status==PXA_STATUS_NOT_FOUND && !(config->capabilities&PXA_VOLUME_WRITE) &&
        config->filesystem_identity && config->filesystem_identity_bytes) {
        uint8_t hash[32];
        if(!sha256(config->filesystem_identity,config->filesystem_identity_bytes,hash))return PXA_STATUS_INTERNAL;
        memcpy(id->bytes,hash,16);*strength=PXA_VOLUME_ID_WEAK;return PXA_STATUS_OK;
    }
    return status;
}
pxa_status_t pxa_esp_storage_register_volume(const pxa_esp_storage_volume_config_t *config,
    uint64_t *epoch) {
    pxa_status_t status;pxa_volume_info_t info={0};uint8_t hash[32];
    char *owned=NULL;unsigned slot;size_t length;
    if(epoch)*epoch=0;
    if(!config || config->struct_size<sizeof(*config) || !epoch ||
        !config->mount_path || config->mount_path[0]!='/' ||
        !config->name || strlen(config->name)>=sizeof(info.name) ||
        !config->device_identity || !config->device_identity_bytes ||
        (config->space_known&~3u) ||
        ((config->space_known&3u)==3u && config->free_bytes>config->total_bytes) ||
        !(config->capabilities&PXA_VOLUME_READ) || !config->link_free ||
        ((config->capabilities&PXA_VOLUME_INSTALLABLE) &&
         (!(config->capabilities&PXA_VOLUME_WRITE) || !config->allocation_unit)))return PXA_STATUS_INVALID_ARGUMENT;
    if((config->capabilities&PXA_VOLUME_INSTALLABLE) && !config->query_space)return PXA_STATUS_INVALID_ARGUMENT;
    length=strnlen(config->mount_path,PXA_ESP_STORAGE_PATH_BYTES);
    if(length<2 || length>=PXA_ESP_STORAGE_PATH_BYTES || config->mount_path[length-1]=='/' ||
        !pxa_fs_scoped_path_is_valid((pxa_bytes_t){(const uint8_t *)config->mount_path+1,length-1}))
        return PXA_STATUS_INVALID_ARGUMENT;
    status=pxa_esp_volumes_initialize();if(status!=PXA_STATUS_OK)return status;
    pxa_esp_volumes_lock();
    for(slot=0;slot<PXA_ESP_STORAGE_MAX_VOLUMES;++slot)
        if(g_mounts[slot].host_path && !strcmp(config->mount_path,g_mounts[slot].host_path))break;
    if(slot<PXA_ESP_STORAGE_MAX_VOLUMES) {status=PXA_STATUS_BUSY;goto done;}
    if(private_for_path(config->mount_path)) {status=PXA_STATUS_DENIED;goto done;}
    for(private_mount_t *private=g_private_mounts;private;private=private->next)
        if(!strncmp(private->path,config->mount_path,length) && private->path[length]=='/') {
            status=PXA_STATUS_DENIED;goto done;
        }
    for(slot=0;slot<PXA_ESP_STORAGE_MAX_VOLUMES;++slot)if(!g_mounts[slot].mount_epoch)break;
    if(slot==PXA_ESP_STORAGE_MAX_VOLUMES) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    if(!sha256(config->device_identity,config->device_identity_bytes,hash)) {status=PXA_STATUS_INTERNAL;goto done;}
    memcpy(info.device_id.bytes,hash,16);
    owned=heap_caps_malloc(length+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!owned)owned=heap_caps_malloc(length+1,MALLOC_CAP_8BIT);
    if(!owned) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    memcpy(owned,config->mount_path,length+1);
    status=local_identity(config,&info.id,&info.identity_strength);if(status!=PXA_STATUS_OK)goto done;
    info.capabilities=config->capabilities;info.total_bytes=config->total_bytes;
    info.free_bytes=config->free_bytes;info.space_known=config->space_known;
    info.max_file_bytes=config->max_file_bytes;info.allocation_unit=config->allocation_unit;
    info.state=PXA_VOLUME_READY;strcpy(info.name,config->name);
    if(config->query_space) {
        status=config->query_space(config->context,owned,&info.total_bytes,&info.free_bytes,&info.space_known);
        /* Capacity discovery is separate from readable media availability.
         * Unknown free space blocks installation, not existing data reads. */
        if(status!=PXA_STATUS_OK) {info.total_bytes=0;info.free_bytes=0;info.space_known=0;}
    }
    status=pxa_volume_register(g_registry,&info,epoch);
    if(status==PXA_STATUS_OK) {
        g_mounts[slot]=(pxa_scoped_fs_mount_t){*epoch,owned,1};owned=NULL;
        g_drivers[slot]=(volume_driver_t){config->context,config->query_space,config->sync_directory};
    }
done:
    heap_caps_free(owned);pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *info) {
    pxa_status_t status;pxa_esp_volumes_lock();
    status=g_registry?pxa_volume_get(g_registry,epoch,info):PXA_STATUS_UNAVAILABLE;
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_find_volume(const pxa_volume_id_t *id,pxa_volume_info_t *info) {
    pxa_status_t status;pxa_esp_volumes_lock();
    status=g_registry?pxa_volume_find(g_registry,id,info):PXA_STATUS_UNAVAILABLE;
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_volume_path(uint64_t epoch,char *output,size_t capacity) {
    pxa_status_t status;pxa_volume_info_t info;int slot;
    if(!output || !capacity)return PXA_STATUS_INVALID_ARGUMENT;
    output[0]=0;pxa_esp_volumes_lock();slot=slot_for_epoch(epoch);
    status=g_registry?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK && (info.ambiguous || info.state!=PXA_VOLUME_READY))status=PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK) {
        if(slot<0)status=PXA_STATUS_UNAVAILABLE;
        else if(strlen(g_mounts[slot].host_path)>=capacity)status=PXA_STATUS_LIMIT_EXCEEDED;
        else strcpy(output,g_mounts[slot].host_path);
    }
    pxa_esp_volumes_unlock();return status;
}
size_t pxa_esp_storage_volumes(pxa_volume_info_t *output,size_t capacity) {
    uint16_t cursor=0;size_t count=0;uint64_t revision;
    if(!output)return 0;
    pxa_esp_volumes_lock();revision=pxa_volume_revision(g_registry);
    while(count<capacity && pxa_volume_next(g_registry,revision,&cursor,&output[count])==PXA_STATUS_OK)++count;
    pxa_esp_volumes_unlock();return count;
}
pxa_status_t pxa_esp_storage_refresh_volume(uint64_t epoch) {
    pxa_volume_info_t info;pxa_status_t status;int slot;
    pxa_esp_volumes_lock();slot=slot_for_epoch(epoch);
    status=g_registry?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK && (slot<0 || info.state!=PXA_VOLUME_READY || info.ambiguous))status=PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK) {
        if(!g_drivers[slot].query_space)status=PXA_STATUS_UNSUPPORTED;
        else {
            uint64_t total=0,free_bytes=0;uint8_t known=0;
            status=g_drivers[slot].query_space(g_drivers[slot].context,g_mounts[slot].host_path,
                &total,&free_bytes,&known);
            if(status==PXA_STATUS_OK)status=pxa_volume_update(g_registry,epoch,info.state,
                total,free_bytes,known);
            if(status!=PXA_STATUS_OK)(void)pxa_volume_update(g_registry,epoch,info.state,
                info.total_bytes,0,info.space_known&PXA_VOLUME_TOTAL_KNOWN);
        }
    }
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_drain_volume(uint64_t epoch) {
    pxa_volume_info_t info;pxa_status_t status;
    pxa_esp_volumes_lock();status=g_registry?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK)status=pxa_volume_update(g_registry,epoch,PXA_VOLUME_DRAINING,
        info.total_bytes,info.free_bytes,info.space_known);
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_unregister_volume(uint64_t epoch) {
    pxa_volume_info_t info;pxa_status_t status;int slot;
    pxa_esp_volumes_lock();slot=slot_for_epoch(epoch);
    status=g_registry?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK && info.state!=PXA_VOLUME_DRAINING)status=PXA_STATUS_BAD_STATE;
    if(status==PXA_STATUS_OK && slot>=0 && g_references[slot])status=PXA_STATUS_BUSY;
    if(status==PXA_STATUS_OK)
        for(private_mount_t *private=g_private_mounts;private;private=private->next)
            if(private->epoch==epoch) {status=PXA_STATUS_BUSY;break;}
    if(status==PXA_STATUS_OK)status=pxa_volume_unregister(g_registry,epoch);
    if(status==PXA_STATUS_OK && slot>=0) {
        heap_caps_free((void *)g_mounts[slot].host_path);
        memset(&g_mounts[slot],0,sizeof(g_mounts[slot]));memset(&g_drivers[slot],0,sizeof(g_drivers[slot]));
    }
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_volumes_retain(uint64_t epoch) {
    pxa_volume_info_t info;pxa_status_t status;int slot;
    pxa_esp_volumes_lock();slot=slot_for_epoch(epoch);
    status=slot>=0?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK && (info.state!=PXA_VOLUME_READY || info.ambiguous))status=PXA_STATUS_UNAVAILABLE;
    if(status==PXA_STATUS_OK) {
        if(g_references[slot]==UINT32_MAX)status=PXA_STATUS_RESOURCE_LIMIT;
        else ++g_references[slot];
    }
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_volumes_release(uint64_t epoch) {
    pxa_status_t status=PXA_STATUS_OK;
    pxa_esp_volumes_lock();int slot=slot_for_epoch(epoch);
    if(slot<0)status=PXA_STATUS_UNAVAILABLE;
    else if(!g_references[slot])status=PXA_STATUS_BAD_STATE;
    else --g_references[slot];
    pxa_esp_volumes_unlock();return status;
}
static pxa_status_t bind_private(const char *path,uint64_t epoch,uint8_t virtual_mount) {
    pxa_volume_info_t info;pxa_status_t status;private_mount_t *binding;size_t length;unsigned count=0;
    if(!path || path[0]!='/' || !epoch)return PXA_STATUS_INVALID_ARGUMENT;
    length=strnlen(path,PXA_FS_SCOPED_MAX_PATH_BYTES);
    if(length<2 || length>=PXA_FS_SCOPED_MAX_PATH_BYTES ||
        !pxa_fs_scoped_path_is_valid((pxa_bytes_t){(const uint8_t *)path+1,length-1}))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_esp_volumes_lock();
    status=g_registry?pxa_volume_get(g_registry,epoch,&info):PXA_STATUS_UNAVAILABLE;
    if(status!=PXA_STATUS_OK)goto done;
    if(info.state!=PXA_VOLUME_READY || info.ambiguous) {status=PXA_STATUS_UNAVAILABLE;goto done;}
    int physical=slot_for_path(path);
    if((virtual_mount && physical>=0) ||
        (!virtual_mount && (physical<0 || g_mounts[physical].mount_epoch!=epoch))) {
        status=PXA_STATUS_DENIED;goto done;
    }
    private_mount_t *existing=private_for_path(path);
    if(existing) {status=!strcmp(existing->path,path)?PXA_STATUS_BUSY:PXA_STATUS_DENIED;goto done;}
    for(binding=g_private_mounts;binding;binding=binding->next) {
        if(!strncmp(binding->path,path,length) && binding->path[length]=='/') {status=PXA_STATUS_DENIED;goto done;}
        ++count;
    }
    if(count==MAX_PRIVATE_MOUNTS) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    binding=heap_caps_malloc(sizeof(*binding)+length+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!binding)binding=heap_caps_malloc(sizeof(*binding)+length+1,MALLOC_CAP_8BIT);
    if(!binding) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    binding->epoch=epoch;binding->virtual_mount=virtual_mount;memcpy(binding->path,path,length+1);
    binding->next=g_private_mounts;g_private_mounts=binding;
done:
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_volumes_bind_private(const char *path,uint64_t epoch) {
    return bind_private(path,epoch,1);
}
pxa_status_t pxa_esp_volumes_bind_app_home(const char *path,uint64_t epoch) {
    return bind_private(path,epoch,0);
}
pxa_status_t pxa_esp_volumes_unbind_private(const char *path,uint64_t epoch) {
    pxa_status_t status=PXA_STATUS_NOT_FOUND;private_mount_t **link;
    if(!path || path[0]!='/' || !epoch)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_esp_volumes_lock();
    for(link=&g_private_mounts;*link;link=&(*link)->next) {
        private_mount_t *binding=*link;
        if(binding->epoch==epoch && !strcmp(binding->path,path)) {
            *link=binding->next;heap_caps_free(binding);status=PXA_STATUS_OK;break;
        }
    }
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_plan(const pxa_install_requirements_t *requirements,
    uint64_t app_epoch,uint64_t media_epoch,pxa_install_plan_t *plan) {
    pxa_status_t status;
    if(!requirements || !plan || (!app_epoch && media_epoch))return PXA_STATUS_INVALID_ARGUMENT;
    memset(plan,0,sizeof(*plan));pxa_esp_volumes_lock();
    if(!g_registry)status=PXA_STATUS_UNAVAILABLE;
    else {
        for(unsigned i=0;i<PXA_ESP_STORAGE_MAX_VOLUMES;++i) {
            pxa_volume_info_t info;
            uint64_t epoch=g_mounts[i].mount_epoch;
            if(!epoch || pxa_volume_get(g_registry,epoch,&info)!=PXA_STATUS_OK ||
                info.state!=PXA_VOLUME_READY || info.ambiguous ||
                !(info.capabilities&PXA_VOLUME_INSTALLABLE))continue;
            (void)pxa_esp_storage_refresh_volume(epoch);
        }
        status=app_epoch?pxa_install_plan_evaluate(g_registry,requirements,app_epoch,media_epoch,plan):
            pxa_install_plan_select(g_registry,requirements,0,plan);
    }
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_reserve_plan(pxa_install_plan_t *plan) {
    pxa_status_t status=PXA_STATUS_OK;
    if(!plan || !plan->volume_count || plan->volume_count>2)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_esp_volumes_lock();
    if(!g_registry)status=PXA_STATUS_UNAVAILABLE;
    for(unsigned i=0;status==PXA_STATUS_OK && i<plan->volume_count;++i)
        status=pxa_esp_storage_refresh_volume(plan->volumes[i].mount_epoch);
    if(status==PXA_STATUS_OK)status=pxa_install_plan_reserve(g_registry,plan);
    pxa_esp_volumes_unlock();return status;
}
pxa_status_t pxa_esp_storage_release_plan(pxa_install_plan_t *plan) {
    pxa_status_t status;
    pxa_esp_volumes_lock();
    status=g_registry?pxa_install_plan_release(g_registry,plan):PXA_STATUS_UNAVAILABLE;
    pxa_esp_volumes_unlock();return status;
}
static pxa_status_t fat_space(void *context,const char *path,uint64_t *total,uint64_t *free_bytes,uint8_t *known) {
    (void)context;*known=0;
    if(esp_vfs_fat_info(path,total,free_bytes)!=ESP_OK)return PXA_STATUS_IO_ERROR;
    *known=PXA_VOLUME_TOTAL_KNOWN|PXA_VOLUME_FREE_KNOWN;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_storage_register_fat(const char *path,const char *name,
    const void *device,size_t bytes,
    uint64_t total,uint64_t free_bytes,uint8_t known,uint32_t allocation_unit) {
    pxa_esp_storage_volume_config_t config={0};uint64_t epoch;
    config.struct_size=sizeof(config);config.mount_path=path;config.name=name;
    config.device_identity=device;config.device_identity_bytes=bytes;
    config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_REMOVABLE|
        PXA_VOLUME_CASE_INSENSITIVE|PXA_VOLUME_RANDOM_READ;
    if(allocation_unit)config.capabilities|=PXA_VOLUME_INSTALLABLE;
    config.allocation_unit=allocation_unit;
    config.max_file_bytes=sizeof(off_t)>4?UINT32_MAX:INT32_MAX;
    config.total_bytes=total;config.free_bytes=free_bytes;config.space_known=known;
    config.link_free=1;config.query_space=fat_space;
    return pxa_esp_storage_register_volume(&config,&epoch);
}
