#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_image_blockdev.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "pxa/container.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define IMAGE_BLOCK_BYTES 4096u
#define IMAGE_TRANSFER_BYTES 256u
typedef struct {
    esp_blockdev_t device;
    uint64_t epoch;
    int file;
    int directory;
    int spare_file;
    uint32_t extent,spare_extent;
    uint8_t sparse,cached,dirty,failed;
    uint8_t spare_cached,spare_dirty;
    pxa_esp_image_allocation_t allocation;
} image_device_t;

/* Mapped read-only storage: no per-image erase buffer in SRAM or PSRAM. */
#define ERASED_16 255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255
#define ERASED_256 ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16,ERASED_16
static const uint8_t erased_block[IMAGE_BLOCK_BYTES]={
    ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,
    ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256,ERASED_256};
#undef ERASED_256
#undef ERASED_16

static int available(image_device_t *image) {
    pxa_volume_info_t info;
    return pxa_esp_storage_volume(image->epoch,&info)==PXA_STATUS_OK &&
        info.state==PXA_VOLUME_READY && !info.ambiguous &&
        (info.capabilities&(PXA_VOLUME_READ|PXA_VOLUME_WRITE))==(PXA_VOLUME_READ|PXA_VOLUME_WRITE);
}
static int valid_range(image_device_t *image,uint64_t offset,size_t bytes,size_t alignment) {
    return bytes && offset%alignment==0 && bytes%alignment==0 &&
        offset<=image->device.geometry.disk_size && bytes<=image->device.geometry.disk_size-offset;
}
static esp_err_t raw_transfer(int file,void *data,uint64_t offset,size_t bytes,int writing) {
    size_t transferred=0;
    /* This fd is exclusive to the image and LittleFS serializes operations.
     * FatFS pread/pwrite seek back to the saved cursor after every transfer;
     * growing an image from offset zero then repeatedly walks its full chain.
     * Keep the native cursor where the last operation left it instead. */
    off_t position;
    do {position=lseek(file,(off_t)offset,SEEK_SET);} while(position<0 && errno==EINTR);
    if(position<0 || (uint64_t)position!=offset)return ESP_FAIL;
    while(transferred<bytes) {
        ssize_t count=writing?
            write(file,(const uint8_t *)data+transferred,bytes-transferred):
            read(file,(uint8_t *)data+transferred,bytes-transferred);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return ESP_FAIL;
        transferred+=(size_t)count;
    }
    return ESP_OK;
}
static size_t extent_bytes(image_device_t *image,uint32_t index) {
    uint64_t left=image->device.geometry.disk_size-(uint64_t)index*PXA_ESP_IMAGE_EXTENT_BYTES;
    return left<PXA_ESP_IMAGE_EXTENT_BYTES?(size_t)left:(size_t)PXA_ESP_IMAGE_EXTENT_BYTES;
}
static esp_err_t close_extent(image_device_t *image) {
    int failed=0;
    if(image->file>=0) {
        if(image->dirty && fsync(image->file))failed=1;
        if(pxa_esp_close(image->file))failed=1;
    }
    image->file=-1;image->cached=image->dirty=0;
    if(failed)image->failed=1;
    return failed?ESP_FAIL:ESP_OK;
}
static pxa_status_t recover_extents(int directory,uint64_t capacity) {
    int fd=pxa_esp_openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW,0);
    if(fd<0)return PXA_STATUS_IO_ERROR;
    DIR *scan=pxa_esp_fdopendir(fd);
    if(!scan) {pxa_esp_close(fd);return PXA_STATUS_IO_ERROR;}
    struct dirent *entry;pxa_status_t status=PXA_STATUS_OK;size_t entries=0;
    const uint64_t blocks=(capacity+PXA_ESP_IMAGE_EXTENT_BYTES-1)/PXA_ESP_IMAGE_EXTENT_BYTES;
    errno=0;
    while((entry=readdir(scan))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(++entries>blocks*2+1) {status=PXA_STATUS_DENIED;break;}
        if(!strncmp(entry->d_name,".new-",5) && strlen(entry->d_name)==13) {
            uint32_t index=0;unsigned i;
            for(i=5;i<13;++i) {
                char c=entry->d_name[i];
                if(!((c>='0' && c<='9') || (c>='a' && c<='f')))break;
                index=index*16+(unsigned)(c<='9'?c-'0':c-'a'+10);
            }
            if(i!=13 || index>=blocks) {status=PXA_STATUS_DENIED;break;}
            /* An unpublished extent contains only erased bytes; the old
             * filesystem never referenced it. Unlink does not follow links. */
            if(pxa_esp_unlinkat(directory,entry->d_name,0)) {status=PXA_STATUS_IO_ERROR;break;}
        }
        errno=0;
    }
    if(errno && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    if(closedir(scan) && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    return status;
}
static int valid_extent(int fd,size_t size) {
    struct stat st;
    return pxa_esp_fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_nlink<=1 &&
        st.st_size==(off_t)size;
}
static void swap_extents(image_device_t *image) {
    int file=image->file;uint32_t extent=image->extent;
    uint8_t cached=image->cached,dirty=image->dirty;
    image->file=image->spare_file;image->extent=image->spare_extent;
    image->cached=image->spare_cached;image->dirty=image->spare_dirty;
    image->spare_file=file;image->spare_extent=extent;
    image->spare_cached=cached;image->spare_dirty=dirty;
}
static esp_err_t select_extent(image_device_t *image,uint32_t index,int writing) {
    char name[16],pending[20];size_t bytes=extent_bytes(image,index);
    if(image->failed)return ESP_FAIL;
    if(image->cached && image->extent==index && (image->file>=0 || !writing))return ESP_OK;
    /* Keep metadata and data extents open together. One slot forces a FAT
     * close/sync on every alternating LFS read, even before its sync barrier.
     * Two slots add one FD and a few bytes, without caching data in RAM. */
    swap_extents(image);
    if(image->cached && image->extent==index && (image->file>=0 || !writing))return ESP_OK;
    if(close_extent(image)!=ESP_OK)return ESP_FAIL;
    snprintf(name,sizeof(name),"b%08lx",(unsigned long)index);
    int fd=pxa_esp_openat(image->directory,name,O_RDWR|O_CLOEXEC|O_NOFOLLOW,0);
    if(fd<0 && errno!=ENOENT)return ESP_FAIL;
    if(fd<0 && writing) {
        if(!image->allocation.reserve || !image->allocation.release ||
            image->allocation.reserve(image->allocation.context,bytes)!=PXA_STATUS_OK)return ESP_FAIL;
        snprintf(pending,sizeof(pending),".new-%08lx",(unsigned long)index);
        /* A torn allocation never becomes a visible logical block. Its exact
         * reserved temporary name is disposable on the next allocation. */
        int failed=pxa_esp_unlinkat(image->directory,pending,0)!=0 && errno!=ENOENT;
        if(!failed)fd=pxa_esp_openat(image->directory,pending,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)failed=1;
        for(size_t at=0;!failed && at<bytes;at+=IMAGE_BLOCK_BYTES)
            if(!available(image) || raw_transfer(fd,(void *)erased_block,at,IMAGE_BLOCK_BYTES,1)!=ESP_OK)failed=1;
        if(fd>=0) {
            if(!failed && fsync(fd))failed=1;
            if(pxa_esp_close(fd))failed=1;
            fd=-1;
        }
        /* Persist the extent and its name before allowing LittleFS to program
         * any bytes. Subsequent writes never resize FAT files. */
        if(!failed && pxa_esp_renameat(image->directory,pending,image->directory,name))failed=1;
        if(!failed && pxa_esp_fsync(image->directory))failed=1;
        image->allocation.release(image->allocation.context);
        if(failed) {image->failed=1;pxa_esp_unlinkat(image->directory,pending,0);return ESP_FAIL;}
        fd=pxa_esp_openat(image->directory,name,O_RDWR|O_CLOEXEC|O_NOFOLLOW,0);
        if(fd<0)return ESP_FAIL;
    }
    if(fd>=0 && !valid_extent(fd,bytes)) {pxa_esp_close(fd);return ESP_FAIL;}
    image->file=fd;image->extent=index;image->cached=1;
    return ESP_OK;
}
static esp_err_t transfer(image_device_t *image,void *data,uint64_t offset,size_t bytes,int writing) {
    if(!available(image))return ESP_ERR_INVALID_STATE;
    if(!image->sparse)return raw_transfer(image->file,data,offset,bytes,writing);
    for(size_t at=0;at<bytes;) {
        uint32_t index=(uint32_t)((offset+at)/PXA_ESP_IMAGE_EXTENT_BYTES);
        size_t local=(size_t)((offset+at)%PXA_ESP_IMAGE_EXTENT_BYTES),amount=bytes-at;
        if(amount>PXA_ESP_IMAGE_EXTENT_BYTES-local)amount=PXA_ESP_IMAGE_EXTENT_BYTES-local;
        if(select_extent(image,index,writing)!=ESP_OK)return ESP_FAIL;
        if(image->file<0)memset((uint8_t *)data+at,255,amount);
        else {
            if(writing)image->dirty=1;
            if(raw_transfer(image->file,(uint8_t *)data+at,local,amount,writing)!=ESP_OK) {
                image->failed=1;return ESP_FAIL;
            }
        }
        at+=amount;
    }
    return ESP_OK;
}
static esp_err_t image_read(esp_blockdev_handle_t device,uint8_t *output,size_t capacity,uint64_t offset,size_t bytes) {
    image_device_t *image=device->ctx;
    if(!output || capacity<bytes || !valid_range(image,offset,bytes,IMAGE_TRANSFER_BYTES))return ESP_ERR_INVALID_ARG;
    return transfer(image,output,offset,bytes,0);
}
static esp_err_t image_write(esp_blockdev_handle_t device,const uint8_t *input,uint64_t offset,size_t bytes) {
    image_device_t *image=device->ctx;
    if(!input || !valid_range(image,offset,bytes,IMAGE_TRANSFER_BYTES))return ESP_ERR_INVALID_ARG;
    return transfer(image,(void *)input,offset,bytes,1);
}
static esp_err_t image_erase(esp_blockdev_handle_t device,uint64_t offset,size_t bytes) {
    image_device_t *image=device->ctx;
    if(!valid_range(image,offset,bytes,IMAGE_BLOCK_BYTES))return ESP_ERR_INVALID_ARG;
    for(size_t done=0;done<bytes;done+=IMAGE_BLOCK_BYTES) {
        if(image->sparse) {
            if(!available(image))return ESP_ERR_INVALID_STATE;
            if(select_extent(image,(uint32_t)((offset+done)/PXA_ESP_IMAGE_EXTENT_BYTES),0)!=ESP_OK)return ESP_FAIL;
            if(image->file<0)continue; /* Unallocated bytes are already erased. */
        }
        esp_err_t status=transfer(image,(void *)erased_block,offset+done,IMAGE_BLOCK_BYTES,1);
        if(status!=ESP_OK)return status;
    }
    return ESP_OK;
}
static esp_err_t image_sync(esp_blockdev_handle_t device) {
    image_device_t *image=device->ctx;
    if(!available(image))return ESP_ERR_INVALID_STATE;
    if(image->failed)return ESP_FAIL;
    if(image->sparse) {
        if((image->file>=0 && image->dirty && fsync(image->file)) ||
           (image->spare_file>=0 && image->spare_dirty && fsync(image->spare_file))) {
            image->failed=1;return ESP_FAIL;
        }
    } else if(fsync(image->file)) {image->failed=1;return ESP_FAIL;}
    image->dirty=image->spare_dirty=0;return ESP_OK;
}
static esp_err_t image_ioctl(esp_blockdev_handle_t device,const uint8_t command,void *arguments) {
    image_device_t *image=device->ctx;
    esp_blockdev_cmd_arg_erase_t *range=arguments;
    if(command!=ESP_BLOCKDEV_CMD_ERASE_CONTENTS && command!=ESP_BLOCKDEV_CMD_MARK_DELETED)return ESP_ERR_NOT_SUPPORTED;
    if(!range || !valid_range(image,range->start_addr,range->erase_len,IMAGE_BLOCK_BYTES))return ESP_ERR_INVALID_ARG;
    if(!available(image))return ESP_ERR_INVALID_STATE;
    return command==ESP_BLOCKDEV_CMD_MARK_DELETED?ESP_OK:image_erase(device,range->start_addr,range->erase_len);
}
static esp_err_t image_release(esp_blockdev_handle_t device) {
    image_device_t *image=device->ctx;
    esp_err_t status=image_sync(device);
    if(image->file>=0 && pxa_esp_close(image->file)!=0 && status==ESP_OK)status=ESP_FAIL;
    if(image->sparse && image->spare_file>=0 && pxa_esp_close(image->spare_file)!=0 && status==ESP_OK)status=ESP_FAIL;
    if(image->sparse && pxa_esp_close(image->directory)!=0 && status==ESP_OK)status=ESP_FAIL;
    heap_caps_free(image);
    return status;
}
static const esp_blockdev_ops_t operations={
    .read=image_read,.write=image_write,.erase=image_erase,.sync=image_sync,
    .ioctl=image_ioctl};

pxa_status_t pxa_esp_image_blockdev_close(esp_blockdev_handle_t *owner) {
    esp_err_t status;esp_blockdev_handle_t device;
    if(!owner)return PXA_STATUS_INVALID_ARGUMENT;
    if(!*owner)return PXA_STATUS_OK;
    device=*owner;*owner=NULL;status=image_release(device);
    return status==ESP_OK?PXA_STATUS_OK:status==ESP_ERR_INVALID_STATE?PXA_STATUS_UNAVAILABLE:PXA_STATUS_IO_ERROR;
}

pxa_status_t pxa_esp_image_blockdev_prepare(int file,uint64_t epoch,uint64_t capacity) {
    image_device_t image={0};struct stat metadata;int flags;
    if(file<0 || !epoch || capacity<16*IMAGE_BLOCK_BYTES || capacity>INT32_MAX ||
        capacity%IMAGE_BLOCK_BYTES)return PXA_STATUS_INVALID_ARGUMENT;
    flags=fcntl(file,F_GETFL);
    if(flags<0 || pxa_esp_fstat(file,&metadata)!=0)return PXA_STATUS_IO_ERROR;
    if((flags&O_ACCMODE)!=O_RDWR || (flags&O_APPEND) || !S_ISREG(metadata.st_mode) ||
        metadata.st_nlink>1 || metadata.st_size)return PXA_STATUS_DENIED;
    image.file=file;image.epoch=epoch;image.device.geometry.disk_size=capacity;
    if(!available(&image))return PXA_STATUS_UNAVAILABLE;
    /* A new private image is a sequential fill, often many MiB. Batch those
     * writes on FAT/SD instead of paying the per-write cost every 4 KiB.
     * This scratch exists only during creation; low memory retains the
     * original bounded path and never borrows extra internal SRAM. */
    const size_t batch_bytes=65536;
    uint8_t *batch=capacity>batch_bytes?heap_caps_calloc(1,batch_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    if(batch)memset(batch,255,batch_bytes);
    const size_t step=batch?batch_bytes:IMAGE_BLOCK_BYTES;
    pxa_status_t result=PXA_STATUS_OK;
    for(uint64_t offset=0;offset<capacity;) {
        size_t bytes=capacity-offset<step?(size_t)(capacity-offset):step;
        esp_err_t status=transfer(&image,batch?batch:(void *)erased_block,offset,bytes,1);
        if(status!=ESP_OK) {
            result=status==ESP_ERR_INVALID_STATE?PXA_STATUS_UNAVAILABLE:PXA_STATUS_IO_ERROR;
            break;
        }
        offset+=bytes;
#ifdef ESP_PLATFORM
        if(offset%65536u==0)vTaskDelay(1);
#endif
    }
    heap_caps_free(batch);
    if(result!=PXA_STATUS_OK)return result;
    if(!available(&image))return PXA_STATUS_UNAVAILABLE;
    return fsync(file)==0?PXA_STATUS_OK:PXA_STATUS_IO_ERROR;
}

pxa_status_t pxa_esp_image_blockdev_take(int file,uint64_t epoch,esp_blockdev_handle_t *output) {
    struct stat metadata;image_device_t *image;int flags;
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;
    if(file<0 || !epoch)return PXA_STATUS_INVALID_ARGUMENT;
    flags=fcntl(file,F_GETFL);
    if(flags<0)return PXA_STATUS_IO_ERROR;
    if((flags&O_ACCMODE)!=O_RDWR || (flags&O_APPEND))return PXA_STATUS_DENIED;
    if(pxa_esp_fstat(file,&metadata)!=0)return PXA_STATUS_IO_ERROR;
    if(!S_ISREG(metadata.st_mode) || metadata.st_nlink>1 ||
        metadata.st_size<(off_t)(16*IMAGE_BLOCK_BYTES) ||
        (uint64_t)metadata.st_size>INT32_MAX || metadata.st_size%IMAGE_BLOCK_BYTES)
        return PXA_STATUS_DENIED;
    image=heap_caps_calloc(1,sizeof(*image),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!image)image=heap_caps_calloc(1,sizeof(*image),MALLOC_CAP_8BIT);
    if(!image)return PXA_STATUS_RESOURCE_LIMIT;
    image->file=file;image->epoch=epoch;
    if(!available(image)) {heap_caps_free(image);return PXA_STATUS_UNAVAILABLE;}
    image->device.ctx=image;image->device.ops=&operations;
    image->device.device_flags.erase_before_write=1;
    image->device.device_flags.default_val_after_erase=1;
    image->device.geometry=(esp_blockdev_geometry_t){
        .disk_size=(uint64_t)metadata.st_size,.read_size=IMAGE_TRANSFER_BYTES,
        .write_size=IMAGE_TRANSFER_BYTES,.erase_size=IMAGE_BLOCK_BYTES,
        .recommended_read_size=IMAGE_BLOCK_BYTES,.recommended_write_size=IMAGE_BLOCK_BYTES,
        .recommended_erase_size=IMAGE_BLOCK_BYTES};
    *output=&image->device;return PXA_STATUS_OK;
}

pxa_status_t pxa_esp_image_blockdev_prepare_sparse(int directory,uint64_t epoch,uint64_t capacity) {
    uint8_t header[64]={0};struct stat st;int fd;
    if(directory<0 || !epoch || capacity<65536 || capacity>INT32_MAX || capacity%4096)
        return PXA_STATUS_INVALID_ARGUMENT;
    if(pxa_esp_fstat(directory,&st) || !S_ISDIR(st.st_mode))return PXA_STATUS_DENIED;
    memcpy(header,"PXABLK1",7);pxa_write_u64(header+8,capacity);
    pxa_write_u32(header+16,(uint32_t)PXA_ESP_IMAGE_EXTENT_BYTES);
    pxa_write_u32(header+60,pxa_container_crc32(header,60));
    fd=pxa_esp_openat(directory,"image.meta",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return PXA_STATUS_IO_ERROR;
    int failed=raw_transfer(fd,header,0,sizeof(header),1)!=ESP_OK || fsync(fd);
    if(pxa_esp_close(fd))failed=1;
    if(failed)return PXA_STATUS_IO_ERROR;
    /* The initial superblock pair needs only the first extent. This allocation
     * has already been included in the install plan / creation reservation. */
    fd=pxa_esp_openat(directory,"b00000000",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return PXA_STATUS_IO_ERROR;
    pxa_status_t status=pxa_esp_image_blockdev_prepare(fd,epoch,PXA_ESP_IMAGE_EXTENT_BYTES);
    if(pxa_esp_close(fd) && status==PXA_STATUS_OK)status=PXA_STATUS_IO_ERROR;
    if(status==PXA_STATUS_OK && pxa_esp_fsync(directory))status=PXA_STATUS_IO_ERROR;
    return status;
}
pxa_status_t pxa_esp_image_blockdev_take_sparse(int directory,uint64_t epoch,
    const pxa_esp_image_allocation_t *allocation,esp_blockdev_handle_t *output) {
    uint8_t header[64];struct stat st;
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;
    if(directory<0 || !epoch || !allocation)return PXA_STATUS_INVALID_ARGUMENT;
    if(pxa_esp_fstat(directory,&st) || !S_ISDIR(st.st_mode))return PXA_STATUS_DENIED;
    int fd=pxa_esp_openat(directory,"image.meta",O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0);
    if(fd<0)return PXA_STATUS_DENIED;
    int valid=valid_extent(fd,sizeof(header)) && raw_transfer(fd,header,0,sizeof(header),0)==ESP_OK;
    if(pxa_esp_close(fd))valid=0;
    if(!valid || memcmp(header,"PXABLK1\0",8) || pxa_read_u32(header+16)!=PXA_ESP_IMAGE_EXTENT_BYTES ||
       pxa_read_u32(header+60)!=pxa_container_crc32(header,60))return PXA_STATUS_DENIED;
    for(unsigned i=20;i<60;++i)if(header[i])return PXA_STATUS_DENIED;
    uint64_t capacity=pxa_read_u64(header+8);
    if(capacity<65536 || capacity>INT32_MAX || capacity%4096)return PXA_STATUS_DENIED;
    image_device_t *image=heap_caps_calloc(1,sizeof(*image),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!image)image=heap_caps_calloc(1,sizeof(*image),MALLOC_CAP_8BIT);
    if(!image)return PXA_STATUS_RESOURCE_LIMIT;
    image->file=image->spare_file=-1;image->directory=directory;image->epoch=epoch;image->sparse=1;image->allocation=*allocation;
    if(!available(image)) {heap_caps_free(image);return PXA_STATUS_UNAVAILABLE;}
    pxa_status_t recovered=recover_extents(directory,capacity);
    if(recovered!=PXA_STATUS_OK) {heap_caps_free(image);return recovered;}
    /* Re-establish the namespace barrier after an interrupted extent publish,
     * before an existing extent can be programmed by a fresh mount. */
    if(pxa_esp_fsync(directory)) {heap_caps_free(image);return PXA_STATUS_IO_ERROR;}
    image->device.ctx=image;image->device.ops=&operations;
    image->device.device_flags.erase_before_write=1;image->device.device_flags.default_val_after_erase=1;
    image->device.geometry=(esp_blockdev_geometry_t){
        .disk_size=capacity,.read_size=IMAGE_TRANSFER_BYTES,.write_size=IMAGE_TRANSFER_BYTES,.erase_size=IMAGE_BLOCK_BYTES,
        .recommended_read_size=IMAGE_BLOCK_BYTES,.recommended_write_size=IMAGE_BLOCK_BYTES,.recommended_erase_size=IMAGE_BLOCK_BYTES};
    *output=&image->device;return PXA_STATUS_OK;
}
