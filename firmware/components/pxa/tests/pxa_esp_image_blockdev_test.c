#define _GNU_SOURCE
#include "pxa_esp_image_blockdev.h"
#include "pxa/pxa_esp_storage.h"
#include "lfs.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t epoch=10;
static uint8_t state=PXA_VOLUME_READY;
static unsigned live_allocations;
static int fail_batch;
static int writes_before_cut=-1;
static int io_cut=-1,fail_directory_sync;
static unsigned sync_calls;
static void mutation(void) {if(io_cut==0)_Exit(0);if(io_cut>0)--io_cut;}
ssize_t __real_write(int fd,const void *data,size_t size);
int __real_fsync(int fd);
ssize_t __wrap_write(int fd,const void *data,size_t size) {mutation();return __real_write(fd,data,size);}
int __wrap_fsync(int fd) {++sync_calls;mutation();return __real_fsync(fd);}
void *heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    (void)caps;if(fail_batch && n*bytes==65536)return NULL;
    void *p=calloc(n,bytes);if(p)++live_allocations;return p;
}
void heap_caps_free(void *memory) {if(memory) {assert(live_allocations);--live_allocations;}free(memory);}
int pxa_esp_fstat(int file,struct stat *metadata) {return fstat(file,metadata);}
int pxa_esp_close(int fd) {return close(fd);}
DIR *pxa_esp_fdopendir(int fd) {return fdopendir(fd);}
int pxa_esp_openat(int dir,const char *name,int flags,...) {
    mode_t mode=0;if(flags&O_CREAT) {va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
    return openat(dir,name,flags,mode);
}
int pxa_esp_fsync(int fd) {
    if(fail_directory_sync) {errno=EIO;return -1;}return fsync(fd);
}
int pxa_esp_renameat(int from,const char *name,int to,const char *destination) {
    mutation();return renameat(from,name,to,destination);
}
int pxa_esp_unlinkat(int fd,const char *name,int flags) {mutation();return unlinkat(fd,name,flags);}
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
static int deny_allocation;
static unsigned reservations,releases;
static pxa_status_t reserve_sparse(void *ctx,uint64_t bytes) {
    (void)ctx;assert(bytes && bytes<=65536);
    if(deny_allocation)return PXA_STATUS_QUOTA_EXCEEDED;
    ++reservations;return PXA_STATUS_OK;
}
static void release_sparse(void *ctx) {(void)ctx;++releases;assert(releases<=reservations);}
static const pxa_esp_image_allocation_t allocation={NULL,reserve_sparse,release_sparse};
static uint64_t sparse_size(int dir) {
    int fd=openat(dir,".",O_RDONLY|O_DIRECTORY);assert(fd>=0);
    DIR *scan=fdopendir(fd);assert(scan);struct dirent *entry;uint64_t total=0;
    while((entry=readdir(scan))) {
        if(entry->d_name[0]=='.')continue;
        struct stat st;assert(!fstatat(dir,entry->d_name,&st,AT_SYMLINK_NOFOLLOW));
        assert(S_ISREG(st.st_mode));total+=(uint64_t)st.st_size;
    }
    assert(!closedir(scan));return total;
}
static void remove_sparse(int dir,const char *path) {
    int fd=openat(dir,".",O_RDONLY|O_DIRECTORY);assert(fd>=0);
    DIR *scan=fdopendir(fd);assert(scan);struct dirent *entry;
    while((entry=readdir(scan))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        assert(!unlinkat(dir,entry->d_name,0));
    }
    assert(!closedir(scan) && !close(dir) && !rmdir(path));
}
static esp_blockdev_handle_t sparse_open(int dir) {
    esp_blockdev_handle_t device=NULL;int owner=openat(dir,".",O_RDONLY|O_DIRECTORY);assert(owner>=0);
    assert(!pxa_esp_image_blockdev_take_sparse(owner,epoch,&allocation,&device));return device;
}
static struct lfs_config sparse_config(esp_blockdev_handle_t device) {
    struct lfs_config config={0};config.context=device;
    config.read=fs_read;config.prog=fs_write;config.erase=fs_erase;config.sync=fs_sync;
    config.read_size=256;config.prog_size=256;config.block_size=4096;
    config.block_count=(lfs_size_t)(device->geometry.disk_size/4096);
    config.cache_size=512;config.lookahead_size=16;config.block_cycles=-1;return config;
}
static void test_sparse(void) {
    char path[512];uint8_t data[4096];struct stat st;
    snprintf(path,sizeof(path),"%s/sparse-image-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp");
    assert(mkdtemp(path));int dir=open(path,O_RDONLY|O_DIRECTORY);assert(dir>=0);
    assert(!pxa_esp_image_blockdev_prepare_sparse(dir,epoch,16*1024*1024));
    assert(sparse_size(dir)==65536+64 && !live_allocations);
    esp_blockdev_handle_t device=sparse_open(dir);assert(live_allocations==1);
    assert(!device->ops->read(device,data,sizeof(data),8*1024*1024,sizeof(data)));
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==255);
    assert(!device->ops->erase(device,8*1024*1024,sizeof(data)) && sparse_size(dir)==65536+64);
    deny_allocation=1;
    assert(device->ops->write(device,data,8*1024*1024,sizeof(data))!=ESP_OK);
    assert(!device->ops->sync(device) && sparse_size(dir)==65536+64);
    deny_allocation=0;memset(data,42,sizeof(data));
    assert(!device->ops->write(device,data,8*1024*1024,sizeof(data)));
    assert(!device->ops->sync(device) && sparse_size(dir)==2*65536+64);
    /* Metadata/data alternation must not introduce an implicit disk barrier. */
    assert(!device->ops->read(device,data,sizeof(data),0,sizeof(data)));
    unsigned synced=sync_calls;
    for(unsigned i=0;i<8;++i)
        assert(!device->ops->write(device,data,i%2?8*1024*1024:0,256));
    assert(sync_calls==synced);
    assert(!device->ops->sync(device) && sync_calls==synced+2);
    memset(data,42,sizeof(data));assert(!device->ops->write(device,data,8*1024*1024,sizeof(data)));
    assert(reservations==releases && live_allocations==1);
    assert(!pxa_esp_image_blockdev_close(&device));device=sparse_open(dir);
    memset(data,0,sizeof(data));assert(!device->ops->read(device,data,sizeof(data),8*1024*1024,sizeof(data)));
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==42);
    /* A failed namespace barrier cannot be bypassed by retrying the write. */
    fail_directory_sync=1;
    assert(device->ops->write(device,data,9*1024*1024,sizeof(data))!=ESP_OK);
    fail_directory_sync=0;
    assert(device->ops->write(device,data,9*1024*1024,sizeof(data))!=ESP_OK);
    assert(pxa_esp_image_blockdev_close(&device)==PXA_STATUS_IO_ERROR && !live_allocations);
    device=sparse_open(dir);
    assert(!device->ops->write(device,data,9*1024*1024,sizeof(data)));
    assert(!pxa_esp_image_blockdev_close(&device));
    /* Existing truncated extents fail closed and are never resized. */
    int file=openat(dir,"b00000080",O_WRONLY);assert(file>=0 && !ftruncate(file,512) && !close(file));
    device=sparse_open(dir);
    assert(device->ops->read(device,data,sizeof(data),8*1024*1024,sizeof(data))!=ESP_OK);
    assert(!fstatat(dir,"b00000080",&st,0) && st.st_size==512);
    assert(!pxa_esp_image_blockdev_close(&device));
    remove_sparse(dir,path);
    /* Crash at writes, syncs, extent publication, or LFS rename. Start from
     * a real cold-mounted filesystem each time; preserve the original save. */
    unsigned old=0,updated=0;
    for(int cut=0;cut<86;++cut) {
        snprintf(path,sizeof(path),"%s/sparse-cut-XXXXXX",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp");
        assert(mkdtemp(path));dir=open(path,O_RDONLY|O_DIRECTORY);assert(dir>=0);
        assert(!pxa_esp_image_blockdev_prepare_sparse(dir,epoch,16*1024*1024));
        device=sparse_open(dir);struct lfs_config config=sparse_config(device);lfs_t fs;lfs_file_t file;
        assert(!lfs_format(&fs,&config) && !lfs_mount(&fs,&config));
        write_file(&fs,"Save.sav","original");write_file(&fs,"save.sav","lower");
        assert(!lfs_unmount(&fs) && !pxa_esp_image_blockdev_close(&device));
        pid_t child=fork();assert(child>=0);
        if(!child) {
            device=sparse_open(dir);config=sparse_config(device);assert(!lfs_mount(&fs,&config));
            io_cut=cut<80?cut:cut==85?-1:(128<<(cut-80));
            assert(!lfs_file_open(&fs,&file,"next",LFS_O_CREAT|LFS_O_EXCL|LFS_O_WRONLY));
            memset(data,42,sizeof(data));
            for(unsigned i=0;i<32;++i)assert(lfs_file_write(&fs,&file,data,sizeof(data))==sizeof(data));
            assert(!lfs_file_close(&fs,&file) && !lfs_rename(&fs,"next","Save.sav"));
            _Exit(0);
        }
        int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        device=sparse_open(dir);config=sparse_config(device);assert(!lfs_mount(&fs,&config));
        read_file(&fs,"save.sav","lower");assert(!lfs_file_open(&fs,&file,"Save.sav",LFS_O_RDONLY));
        lfs_soff_t size=lfs_file_size(&fs,&file);
        if(size==8) {++old;assert(lfs_file_read(&fs,&file,data,8)==8 && !memcmp(data,"original",8));}
        else {
            assert(size==131072);++updated;
            for(unsigned i=0;i<32;++i) {
                assert(lfs_file_read(&fs,&file,data,sizeof(data))==sizeof(data));
                for(unsigned j=0;j<sizeof(data);++j)assert(data[j]==42);
            }
        }
        assert(!lfs_file_close(&fs,&file) && !lfs_unmount(&fs) && !pxa_esp_image_blockdev_close(&device));
        assert(sparse_size(dir)<1024*1024 && !live_allocations);remove_sparse(dir,path);
    }
    assert(old && updated && reservations==releases);
    puts("Sparse image: 86 crash boundaries, low space, failed publication, remount, case distinction and bounded allocation passed");
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
    /* Creation must remain correct when its optional batching scratch cannot
     * be allocated. Exercise a final 4-KiB tail as well as full batches. */
    fail_batch=1;
    assert(!pxa_esp_image_blockdev_prepare(native,epoch,17*4096) && !live_allocations);
    assert(pread(native,buffer,sizeof(buffer),16*4096)==sizeof(buffer));
    for(unsigned i=0;i<sizeof(buffer);++i)assert(buffer[i]==255);
    assert(!ftruncate(native,0));fail_batch=0;
    assert(!pxa_esp_image_blockdev_prepare(native,epoch,17*4096) && !live_allocations);
    assert(pread(native,buffer,sizeof(buffer),16*4096)==sizeof(buffer));
    for(unsigned i=0;i<sizeof(buffer);++i)assert(buffer[i]==255);
    assert(!ftruncate(native,0));
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
    test_sparse();
    puts("Private image: actual LittleFS case distinction, interrupted replacement, remount, bounds and mount revocation passed");
    return 0;
}
