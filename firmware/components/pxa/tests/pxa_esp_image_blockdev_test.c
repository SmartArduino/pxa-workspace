#define _GNU_SOURCE
#include "pxa_esp_image_blockdev.h"
#include "pxa/pxa_esp_storage.h"
#include "lfs.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t epoch=10;
static uint8_t state=PXA_VOLUME_READY;
static unsigned live_allocations;
static int writes_before_cut=-1;
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    (void)caps;void *p=calloc(n,bytes);if(p)++live_allocations;return p;
}
void heap_caps_free(void *memory) {if(memory) {assert(live_allocations);--live_allocations;}free(memory);}
int pxa_esp_fstat(int file,struct stat *metadata) {return fstat(file,metadata);}
pxa_status_t pxa_esp_storage_volume(uint64_t requested,pxa_volume_info_t *info) {
    if(requested!=epoch)return PXA_STATUS_UNAVAILABLE;
    memset(info,0,sizeof(*info));info->mount_epoch=epoch;info->state=state;
    info->capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE;return PXA_STATUS_OK;
}
static int fs_read(const struct lfs_config *config,lfs_block_t block,lfs_off_t offset,void *output,lfs_size_t bytes) {
    esp_blockdev_handle_t device=config->context;
    return device->ops->read(device,output,bytes,(uint64_t)block*config->block_size+offset,bytes)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_write(const struct lfs_config *config,lfs_block_t block,lfs_off_t offset,const void *input,lfs_size_t bytes) {
    esp_blockdev_handle_t device=config->context;
    if(writes_before_cut==0)return LFS_ERR_IO;
    if(writes_before_cut>0)--writes_before_cut;
    return device->ops->write(device,input,(uint64_t)block*config->block_size+offset,bytes)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_erase(const struct lfs_config *config,lfs_block_t block) {
    esp_blockdev_handle_t device=config->context;
    return device->ops->erase(device,(uint64_t)block*config->block_size,config->block_size)==ESP_OK?0:LFS_ERR_IO;
}
static int fs_sync(const struct lfs_config *config) {
    esp_blockdev_handle_t device=config->context;
    return device->ops->sync(device)==ESP_OK?0:LFS_ERR_IO;
}
static void write_file(lfs_t *fs,const char *name,const char *data) {
    lfs_file_t file;
    assert(!lfs_file_open(fs,&file,name,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_EXCL));
    assert(lfs_file_write(fs,&file,data,strlen(data))==(lfs_ssize_t)strlen(data));
    assert(!lfs_file_close(fs,&file));
}
static void read_file(lfs_t *fs,const char *name,const char *expected) {
    lfs_file_t file;char data[32]={0};
    assert(!lfs_file_open(fs,&file,name,LFS_O_RDONLY));
    assert(lfs_file_read(fs,&file,data,sizeof(data))==(lfs_ssize_t)strlen(expected));
    assert(!strcmp(data,expected) && !lfs_file_close(fs,&file));
}
static void test_interrupted_replacement(int baseline,const char *path,const struct lfs_config *base_config) {
    unsigned old_versions=0,new_versions=0;
    for(unsigned cut=0;cut<16;++cut) {
        char candidate[512],value[32]={0};uint8_t block[4096];
        esp_blockdev_handle_t device;lfs_t fs;lfs_file_t file;
        int descriptor,status;pid_t child;
        assert(snprintf(candidate,sizeof(candidate),"%s-cut-%02u",path,cut)<(int)sizeof(candidate));
        descriptor=open(candidate,O_CREAT|O_EXCL|O_RDWR,0600);assert(descriptor>=0);
        for(off_t offset=0;offset<32*4096;offset+=4096) {
            assert(pread(baseline,block,sizeof(block),offset)==sizeof(block));
            assert(write(descriptor,block,sizeof(block))==sizeof(block));
        }
        assert(!fsync(descriptor) && !close(descriptor));
        child=fork();assert(child>=0);
        if(!child) {
            struct lfs_config config=*base_config;
            descriptor=open(candidate,O_RDWR);assert(descriptor>=0);
            assert(!pxa_esp_image_blockdev_take(descriptor,epoch,&device));config.context=device;
            assert(!lfs_mount(&fs,&config));
            writes_before_cut=(int)cut;
            status=lfs_file_open(&fs,&file,"transaction.sav",LFS_O_CREAT|LFS_O_WRONLY);
            if(!status) {
                if(lfs_file_write(&fs,&file,"after-cut",9)==9 && !lfs_file_sync(&fs,&file) &&
                    !lfs_file_close(&fs,&file))
                    (void)lfs_rename(&fs,"transaction.sav","Save.sav");
            }
            /* Model loss of the mount and its caches without unmount/close.
             * The parent opens a fresh filesystem from the written image. */
            _Exit(0);
        }
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        descriptor=open(candidate,O_RDWR);assert(descriptor>=0);
        assert(!pxa_esp_image_blockdev_take(descriptor,epoch,&device));
        struct lfs_config config=*base_config;config.context=device;
        assert(!lfs_mount(&fs,&config));
        assert(!lfs_file_open(&fs,&file,"Save.sav",LFS_O_RDONLY));
        assert(lfs_file_read(&fs,&file,value,sizeof(value))>0 && !lfs_file_close(&fs,&file));
        if(!strcmp(value,"replacement"))++old_versions;
        else {assert(!strcmp(value,"after-cut"));++new_versions;}
        read_file(&fs,"save.sav","lower");
        assert(!lfs_unmount(&fs) && !pxa_esp_image_blockdev_close(&device) && !device && !unlink(candidate));
    }
    assert(old_versions && new_versions);
}
int main(void) {
    char path[512];uint8_t buffer[4096];esp_blockdev_handle_t device=NULL;
    struct lfs_config config={0};lfs_t fs;lfs_file_t file;struct stat metadata;
    const char *temporary=getenv("TMPDIR");int native,read_only,append;
    assert(snprintf(path,sizeof(path),"%s/private-image-XXXXXX",temporary?temporary:"/tmp")<(int)sizeof(path));
    native=mkstemp(path);assert(native>=0);
    /* Invalid/newly truncated images are not silently resized or formatted. */
    assert(pxa_esp_image_blockdev_take(native,epoch,&device)==PXA_STATUS_DENIED && !device);
    assert(!fstat(native,&metadata) && !metadata.st_size);
    assert(pxa_esp_image_blockdev_prepare(native,epoch,4096)==PXA_STATUS_INVALID_ARGUMENT);
    state=PXA_VOLUME_DRAINING;
    assert(pxa_esp_image_blockdev_prepare(native,epoch,32*4096)==PXA_STATUS_UNAVAILABLE);
    assert(!fstat(native,&metadata) && !metadata.st_size);
    state=PXA_VOLUME_READY;
    assert(!pxa_esp_image_blockdev_prepare(native,epoch,32*4096) && !live_allocations);
    assert(!fstat(native,&metadata) && metadata.st_size==32*4096);
    assert(pread(native,buffer,sizeof(buffer),31*4096)==sizeof(buffer));
    for(unsigned i=0;i<sizeof(buffer);++i)assert(buffer[i]==255);
    assert(pxa_esp_image_blockdev_prepare(native,epoch,32*4096)==PXA_STATUS_DENIED);
    read_only=open(path,O_RDONLY);assert(read_only>=0);
    assert(pxa_esp_image_blockdev_take(read_only,epoch,&device)==PXA_STATUS_DENIED && !device);
    assert(!close(read_only));
    append=open(path,O_RDWR|O_APPEND);assert(append>=0);
    assert(pxa_esp_image_blockdev_take(append,epoch,&device)==PXA_STATUS_DENIED && !device);
    assert(!close(append));
    assert(!pxa_esp_image_blockdev_take(native,epoch,&device) && device && live_allocations==1);
    assert(!device->ops->release); /* ESP LittleFS always borrows the device. */
    assert(device->ops->read(device,buffer,255,0,256)==ESP_ERR_INVALID_ARG);
    assert(device->ops->write(device,buffer,1,256)==ESP_ERR_INVALID_ARG);
    assert(device->ops->erase(device,device->geometry.disk_size,4096)==ESP_ERR_INVALID_ARG);
    config.context=device;config.read=fs_read;config.prog=fs_write;config.erase=fs_erase;config.sync=fs_sync;
    config.read_size=256;config.prog_size=256;config.block_size=4096;config.block_count=32;
    config.cache_size=512;config.lookahead_size=16;config.block_cycles=-1;
    assert(lfs_mount(&fs,&config)==LFS_ERR_CORRUPT); /* Opening did not format it. */
    assert(!lfs_format(&fs,&config) && !lfs_mount(&fs,&config));
    write_file(&fs,"Save.sav","upper");write_file(&fs,"save.sav","lower");
    write_file(&fs,"next.sav","replacement");
    assert(!lfs_rename(&fs,"next.sav","Save.sav"));
    read_file(&fs,"Save.sav","replacement");read_file(&fs,"save.sav","lower");
    assert(!lfs_file_open(&fs,&file,"Save.sav",LFS_O_RDWR));
    assert(!lfs_file_sync(&fs,&file) && !lfs_file_close(&fs,&file));
    assert(!lfs_unmount(&fs) && !device->ops->sync(device));
    /* Cold remount retains both exact names and the completed replacement. */
    memset(&fs,0,sizeof(fs));assert(!lfs_mount(&fs,&config));
    read_file(&fs,"Save.sav","replacement");read_file(&fs,"save.sav","lower");
    assert(!lfs_unmount(&fs));
    test_interrupted_replacement(native,path,&config);
    state=PXA_VOLUME_DRAINING;
    assert(device->ops->read(device,buffer,sizeof(buffer),0,256)==ESP_ERR_INVALID_STATE);
    assert(device->ops->write(device,buffer,0,256)==ESP_ERR_INVALID_STATE);
    state=PXA_VOLUME_READY;++epoch;
    assert(device->ops->erase(device,0,4096)==ESP_ERR_INVALID_STATE);
    assert(device->ops->sync(device)==ESP_ERR_INVALID_STATE);
    /* Release always closes the old fd, including after media removal. */
    assert(pxa_esp_image_blockdev_close(&device)==PXA_STATUS_UNAVAILABLE && !device && !live_allocations);
    assert(!pxa_esp_image_blockdev_close(&device));
    assert(fcntl(native,F_GETFD)==-1 && errno==EBADF);
    assert(!unlink(path));
    puts("Private image: actual LittleFS case distinction, interrupted replacement, remount, bounds and mount revocation passed");
    return 0;
}
