#define _GNU_SOURCE
#include "pxa_esp_volumes.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "pxa/posix/pxa_posix_app_store.h"
#include "esp_vfs_fat.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {uint64_t capacity,free_bytes;unsigned queries;int fail;} capacity_t;
static unsigned random_state=1;
void *heap_caps_malloc(size_t bytes,unsigned caps) {(void)caps;return malloc(bytes);}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {(void)caps;return calloc(n,bytes);}
void heap_caps_free(void *memory) {free(memory);}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *free_bytes) {
    (void)path;*total=100000;*free_bytes=90000;return ESP_OK;
}
static pxa_status_t capacity(void *context,const char *path,uint64_t *total,uint64_t *free_bytes,uint8_t *known) {
    capacity_t *value=context;(void)path;++value->queries;
    if(value->fail)return PXA_STATUS_IO_ERROR;
    *total=value->capacity;*free_bytes=value->free_bytes;*known=3;return PXA_STATUS_OK;
}
static pxa_esp_storage_volume_config_t configuration(const char *path,const char *device,capacity_t *space,uint8_t removable) {
    pxa_esp_storage_volume_config_t config={0};
    config.struct_size=sizeof(config);config.mount_path=path;config.name="test volume";
    config.device_identity=device;config.device_identity_bytes=strlen(device);
    config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE|PXA_VOLUME_RANDOM_READ;
    if(removable)config.capabilities|=PXA_VOLUME_REMOVABLE|PXA_VOLUME_CASE_INSENSITIVE;
    config.max_file_bytes=UINT32_MAX;config.allocation_unit=removable?32768:4096;
    config.link_free=1;config.context=space;config.query_space=capacity;
    return config;
}
static void copy_descriptor(const char *from,const char *to) {
    char path[512],bytes[32];int input,output;
    assert(snprintf(path,sizeof(path),"%s/PXA",to)<(int)sizeof(path));assert(!mkdir(path,0700));
    for(unsigned i=0;i<2;++i) {
        snprintf(path,sizeof(path),"%s/PXA/volume.%u.pxv",from,i);input=open(path,O_RDONLY);assert(input>=0);
        assert(read(input,bytes,sizeof(bytes))==sizeof(bytes));assert(!close(input));
        snprintf(path,sizeof(path),"%s/PXA/volume.%u.pxv",to,i);output=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(output>=0);
        assert(write(output,bytes,sizeof(bytes))==sizeof(bytes));assert(!close(output));
    }
}
static void remove_volume(const char *path) {
    char child[512];
    for(unsigned i=0;i<2;++i) {snprintf(child,sizeof(child),"%s/PXA/volume.%u.pxv",path,i);assert(!unlink(child));}
    snprintf(child,sizeof(child),"%s/PXA",path);assert(!rmdir(child));assert(!rmdir(path));
}
static int count_installation(void *context,const pxa_posix_app_home_t *home) {
    unsigned *count=context;
    assert(home->record.generation==1 && !strcmp(strrchr(home->locator,'/')+1,home->record.app_id));
    ++*count;return 1;
}
static void test_volume_discovery(const char *mount,const pxa_volume_info_t *info) {
    static const char *const owners[]={"first","first","second"};
    static const char *const apps[]={"quake","duke","quake"};
    char root[512],path[1024];uint8_t encoded[PXA_INSTALL_RECORD_BYTES];
    pxa_posix_app_store_config_t config={0};size_t invalid;
    assert(snprintf(root,sizeof(root),"%s/PXA",mount)<(int)sizeof(root));
    snprintf(path,sizeof(path),"%s/apps",root);assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/apps/first",root);assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/apps/second",root);assert(!mkdir(path,0700));
    for(unsigned i=0;i<3;++i) {
        pxa_install_record_t record={0};int file;
        record.generation=1;record.flags=PXA_INSTALL_RECORD_ENABLED;
        record.install_id[0]=(uint8_t)(i+1);record.publisher_root[0]=(uint8_t)(i+1);
        record.app_volume=info->id;record.manifest_sha256[0]=1;strcpy(record.app_id,apps[i]);
        assert(!pxa_install_record_encode(&record,encoded));
        snprintf(path,sizeof(path),"%s/apps/%s/%s",root,owners[i],apps[i]);assert(!mkdir(path,0700));
        snprintf(path,sizeof(path),"%s/apps/%s/%s/install.0.pxi",root,owners[i],apps[i]);
        file=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);assert(file>=0);
        assert(write(file,encoded,sizeof(encoded))==sizeof(encoded) && !close(file));
    }
    config.root=root;config.volume_id=info->id;
    /* Run the actual ESP fdopendir facade: it consumes its input descriptor.
     * Discovery must retain independent parent fds for opening app homes. */
    for(unsigned i=0;i<32;++i) {
        unsigned count=0;
        assert(!pxa_posix_app_store_visit(&config,count_installation,&count,&invalid));
        assert(count==3 && invalid==0);
    }
    for(unsigned i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/apps/%s/%s/install.0.pxi",root,owners[i],apps[i]);assert(!unlink(path));
        snprintf(path,sizeof(path),"%s/apps/%s/%s",root,owners[i],apps[i]);assert(!rmdir(path));
    }
    snprintf(path,sizeof(path),"%s/apps/first",root);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/apps/second",root);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/apps",root);assert(!rmdir(path));
}
int main(void) {
    char root[256],internal[256],sd[256],partition[256],clone[256],output[256];
    const char *temporary=getenv("TMPDIR");
    capacity_t flash_space={100000,0,0,0},sd_space={1000000,900000,0,0},part_space={100000,90000,0,0};
    pxa_esp_storage_volume_config_t config;pxa_volume_info_t flash,card,part,other;
    pxa_install_requirements_t requirements={10000,10000,50000,0,4096};pxa_install_plan_t plan={0};
    uint64_t flash_epoch,sd_epoch,part_epoch,clone_epoch,new_epoch;struct stat metadata;int directory;
    assert(snprintf(root,sizeof(root),"%s/volumes-XXXXXX",temporary?temporary:"/tmp")<(int)sizeof(root));assert(mkdtemp(root));
    assert(snprintf(internal,sizeof(internal),"%s/flash",root)<(int)sizeof(internal));assert(!mkdir(internal,0700));
    assert(snprintf(sd,sizeof(sd),"%s/sd",root)<(int)sizeof(sd));assert(!mkdir(sd,0700));
    assert(snprintf(partition,sizeof(partition),"%s/partition",root)<(int)sizeof(partition));assert(!mkdir(partition,0700));
    assert(snprintf(clone,sizeof(clone),"%s/clone",root)<(int)sizeof(clone));assert(!mkdir(clone,0700));
    config=configuration(internal,"flash-chip",&flash_space,0);
    assert(!pxa_esp_storage_register_volume(&config,&flash_epoch));assert(!pxa_esp_storage_volume(flash_epoch,&flash));
    assert(pxa_esp_storage_register_volume(&config,&new_epoch)==PXA_STATUS_BUSY && !new_epoch);
    /* A full primary partition cannot stop a complete installation on SD. */
    config=configuration(sd,"sd-card",&sd_space,1);
    assert(!pxa_esp_storage_register_volume(&config,&sd_epoch));assert(!pxa_esp_storage_volume(sd_epoch,&card));
    uint64_t storage_total,storage_free;size_t storage_volumes;
    assert(!pxa_esp_storage_usage(&storage_total,&storage_free,&storage_volumes) &&
        storage_total==1100000 && storage_free==900000 && storage_volumes==2);
    unsigned queries=flash_space.queries+sd_space.queries;
    assert(!pxa_esp_storage_resolve_volume(sd,&other) && other.mount_epoch==sd_epoch);
    assert(!pxa_esp_storage_usage(&storage_total,&storage_free,&storage_volumes));
    assert(flash_space.queries+sd_space.queries==queries); /* UI/debug reads never scan filesystems. */
    test_volume_discovery(sd,&card);
    assert(!pxa_esp_storage_plan(&requirements,0,0,&plan) && plan.app_mount_epoch==sd_epoch && plan.media_mount_epoch==sd_epoch);
    sd_space.free_bytes=1;
    assert(pxa_esp_storage_reserve_plan(&plan)==PXA_STATUS_QUOTA_EXCEEDED && !plan.reserved);
    sd_space.free_bytes=900000;assert(!pxa_esp_storage_reserve_plan(&plan) && plan.reserved);
    assert(!pxa_esp_storage_volume(sd_epoch,&other) && other.reserved_bytes==74096);
    assert(!pxa_esp_storage_release_plan(&plan));
    config=configuration(partition,"flash-chip",&part_space,0);
    assert(!pxa_esp_storage_register_volume(&config,&part_epoch));assert(!pxa_esp_storage_volume(part_epoch,&part));
    assert(!memcmp(flash.device_id.bytes,part.device_id.bytes,16) && memcmp(flash.id.bytes,part.id.bytes,16));
    assert(!pxa_esp_storage_plan(&requirements,0,0,&plan) && plan.app_mount_epoch==part_epoch);
    part_space.fail=1;
    assert(!pxa_esp_storage_plan(&requirements,0,0,&plan) && plan.app_mount_epoch==sd_epoch);
    assert(!pxa_esp_storage_volume(part_epoch,&other) && other.state==PXA_VOLUME_READY &&
        !(other.space_known&PXA_VOLUME_FREE_KNOWN));
    assert(pxa_esp_storage_usage(&storage_total,&storage_free,&storage_volumes)==PXA_STATUS_UNAVAILABLE &&
        !storage_total && !storage_free && !storage_volumes);
    part_space.fail=0;
    part_space.free_bytes=part_space.capacity+1;
    assert(!pxa_esp_storage_plan(&requirements,0,0,&plan) && plan.app_mount_epoch==sd_epoch);
    assert(!pxa_esp_storage_volume(part_epoch,&other) && !(other.space_known&PXA_VOLUME_FREE_KNOWN));
    part_space.free_bytes=90000;
    /* Duplicate UUIDs quarantine both mounts rather than selecting by order. */
    copy_descriptor(sd,clone);config=configuration(clone,"cloned-card",&sd_space,1);
    directory=pxa_esp_open(sd,O_RDONLY|O_DIRECTORY);assert(directory>=0);
    assert(!pxa_esp_storage_register_volume(&config,&clone_epoch));
    assert(pxa_esp_storage_find_volume(&card.id,&other)==PXA_STATUS_BUSY);
    assert(pxa_esp_fstat(directory,&metadata)==-1 && errno==EIO);
    assert(pxa_esp_storage_volume_path(sd_epoch,output,sizeof(output))==PXA_STATUS_UNAVAILABLE);
    assert(pxa_esp_storage_unregister_volume(clone_epoch)==PXA_STATUS_BAD_STATE);
    assert(!pxa_esp_storage_drain_volume(clone_epoch));assert(!pxa_esp_storage_unregister_volume(clone_epoch));
    assert(!pxa_esp_storage_find_volume(&card.id,&other) && !other.ambiguous);
    assert(!pxa_esp_storage_drain_volume(flash_epoch));assert(!pxa_esp_storage_unregister_volume(flash_epoch));
    assert(!pxa_esp_storage_drain_volume(part_epoch));assert(!pxa_esp_storage_unregister_volume(part_epoch));
    /* The same descriptor and apps can be discovered with no internal volume. */
    assert(!pxa_esp_storage_plan(&requirements,0,0,&plan) && plan.app_mount_epoch==sd_epoch);
    assert(!pxa_esp_storage_reserve_plan(&plan));
    {
        char alias[256],app_home[512];int private_directory;
        assert(snprintf(alias,sizeof(alias),"%s/private",root)<(int)sizeof(alias));assert(!mkdir(alias,0700));
        assert(snprintf(app_home,sizeof(app_home),"%s/app-home",sd)<(int)sizeof(app_home));assert(!mkdir(app_home,0700));
        assert(!pxa_esp_volumes_bind_app_home(app_home,sd_epoch));
        assert(pxa_esp_volumes_bind_app_home(app_home,sd_epoch)==PXA_STATUS_BUSY);
        assert(!pxa_esp_volumes_bind_private(alias,sd_epoch));
        assert(!pxa_esp_storage_resolve_volume(alias,&other) && other.mount_epoch==sd_epoch);
        assert(!pxa_esp_storage_usage(&storage_total,&storage_free,&storage_volumes) && storage_volumes==1);
        assert(pxa_esp_volumes_bind_private(sd,sd_epoch)==PXA_STATUS_DENIED);
        private_directory=pxa_esp_open(alias,O_RDONLY|O_DIRECTORY);assert(private_directory>=0);
        assert(!pxa_esp_storage_drain_volume(sd_epoch));
        assert(pxa_esp_storage_resolve_volume(alias,&other)==PXA_STATUS_UNAVAILABLE && !other.mount_epoch);
        assert(pxa_esp_storage_unregister_volume(sd_epoch)==PXA_STATUS_BUSY);
        assert(pxa_esp_fstat(private_directory,&metadata)==-1 && errno==EIO);
        assert(!pxa_esp_close(private_directory));
        assert(!pxa_esp_volumes_unbind_private(alias,sd_epoch));
        assert(pxa_esp_storage_unregister_volume(sd_epoch)==PXA_STATUS_BUSY);
        assert(!pxa_esp_volumes_unbind_private(app_home,sd_epoch) && !rmdir(app_home));
        assert(!pxa_esp_storage_unregister_volume(sd_epoch) && !rmdir(alias));
    }
    assert(pxa_esp_storage_release_plan(&plan)==PXA_STATUS_UNAVAILABLE && !plan.reserved);
    config=configuration(sd,"sd-card",&sd_space,1);
    assert(!pxa_esp_storage_register_volume(&config,&new_epoch) && new_epoch!=sd_epoch);
    assert(!pxa_esp_storage_find_volume(&card.id,&other) && other.mount_epoch==new_epoch);
    assert(pxa_esp_fstat(directory,&metadata)==-1 && errno==EIO);assert(!pxa_esp_close(directory));
    assert(!pxa_esp_storage_volume_path(new_epoch,output,sizeof(output)) && !strcmp(output,sd));
    assert(!pxa_esp_storage_drain_volume(new_epoch));assert(!pxa_esp_storage_unregister_volume(new_epoch));
    assert(!pxa_esp_storage_volumes(&other,1));
    remove_volume(internal);remove_volume(partition);remove_volume(clone);remove_volume(sd);assert(!rmdir(root));
    puts("ESP volumes: full Flash, partitions, SD-only, refreshed admission, clone quarantine and remount passed");
    return 0;
}
