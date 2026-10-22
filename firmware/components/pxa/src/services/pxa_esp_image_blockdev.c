#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_image_blockdev.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#include "esp_heap_caps.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
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
static esp_err_t transfer(image_device_t *image,void *data,uint64_t offset,size_t bytes,int writing) {
    size_t transferred=0;
    if(!available(image))return ESP_ERR_INVALID_STATE;
    /* This fd is exclusive to the image and LittleFS serializes operations.
     * FatFS pread/pwrite seek back to the saved cursor after every transfer;
     * growing an image from offset zero then repeatedly walks its full chain.
     * Keep the native cursor where the last operation left it instead. */
    off_t position;
    do {position=lseek(image->file,(off_t)offset,SEEK_SET);} while(position<0 && errno==EINTR);
    if(position<0 || (uint64_t)position!=offset)return ESP_FAIL;
    while(transferred<bytes) {
        ssize_t count=writing?
            write(image->file,(const uint8_t *)data+transferred,bytes-transferred):
            read(image->file,(uint8_t *)data+transferred,bytes-transferred);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return ESP_FAIL;
        transferred+=(size_t)count;
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
        esp_err_t status=transfer(image,(void *)erased_block,offset+done,IMAGE_BLOCK_BYTES,1);
        if(status!=ESP_OK)return status;
    }
    return ESP_OK;
}
static esp_err_t image_sync(esp_blockdev_handle_t device) {
    image_device_t *image=device->ctx;
    if(!available(image))return ESP_ERR_INVALID_STATE;
    return fsync(image->file)==0?ESP_OK:ESP_FAIL;
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
    if(close(image->file)!=0 && status==ESP_OK)status=ESP_FAIL;
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
    for(uint64_t offset=0;offset<capacity;offset+=IMAGE_BLOCK_BYTES) {
        esp_err_t status=transfer(&image,(void *)erased_block,offset,IMAGE_BLOCK_BYTES,1);
        if(status!=ESP_OK)return status==ESP_ERR_INVALID_STATE?PXA_STATUS_UNAVAILABLE:PXA_STATUS_IO_ERROR;
#ifdef ESP_PLATFORM
        if((offset+IMAGE_BLOCK_BYTES)%65536u==0)vTaskDelay(1);
#endif
    }
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
