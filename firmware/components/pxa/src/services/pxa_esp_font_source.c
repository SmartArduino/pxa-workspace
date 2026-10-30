#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "pxa/pxa_esp_font_source.h"
#include "pxa_esp_volumes.h"
#include "esp_heap_caps.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "pxa/esp/pxa_esp_posix_shim.h"
#endif

#define FONT_PREFIX "pxa-font:"
#define FONT_PREFIX_BYTES (sizeof(FONT_PREFIX)-1u)
#define FONT_PATH_BYTES 1024u
struct pxa_esp_font_source {uint64_t epoch;char path[];};
static void *font_allocate(size_t size) {
    void *memory=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    return memory?memory:heap_caps_malloc(size,MALLOC_CAP_8BIT);
}
static pxa_status_t ready(uint64_t epoch) {
    pxa_volume_info_t volume;pxa_status_t status=pxa_esp_storage_volume(epoch,&volume);
    return status==PXA_STATUS_OK && volume.state==PXA_VOLUME_READY && !volume.ambiguous &&
        (volume.capabilities&PXA_VOLUME_READ)?PXA_STATUS_OK:PXA_STATUS_UNAVAILABLE;
}
pxa_status_t pxa_esp_font_source_acquire(const char *path,pxa_esp_font_source_t **output) {
    if(!output)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;if(!path || path[0]!='/')return PXA_STATUS_INVALID_ARGUMENT;
    size_t length=strnlen(path,FONT_PATH_BYTES);
    if(length<2 || length>=FONT_PATH_BYTES || !pxa_fs_scoped_path_is_valid(
        (pxa_bytes_t){(const uint8_t *)path+1,length-1}))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_volume_info_t volume;pxa_status_t status=pxa_esp_storage_resolve_volume(path,&volume);
    if(status!=PXA_STATUS_OK)return status;
    status=pxa_esp_volumes_retain(volume.mount_epoch);if(status!=PXA_STATUS_OK)return status;
    const uint64_t epoch=volume.mount_epoch;
    status=pxa_esp_storage_resolve_volume(path,&volume);
    if(status==PXA_STATUS_OK && (volume.mount_epoch!=epoch || ready(epoch)!=PXA_STATUS_OK))status=PXA_STATUS_UNAVAILABLE;
    pxa_esp_font_source_t *source=NULL;
    if(status==PXA_STATUS_OK) {
        source=font_allocate(sizeof(*source)+FONT_PREFIX_BYTES+16+1+length+1);
        if(!source)status=PXA_STATUS_RESOURCE_LIMIT;
    }
    if(status!=PXA_STATUS_OK) {(void)pxa_esp_volumes_release(epoch);return status;}
    source->epoch=epoch;
    snprintf(source->path,FONT_PREFIX_BYTES+16+1+length+1,FONT_PREFIX "%016llx:%s",
        (unsigned long long)epoch,path);
    *output=source;return PXA_STATUS_OK;
}
const char *pxa_esp_font_source_path(const pxa_esp_font_source_t *source) {
    return source?source->path:NULL;
}
pxa_status_t pxa_esp_font_source_release(pxa_esp_font_source_t **source) {
    if(!source)return PXA_STATUS_INVALID_ARGUMENT;
    if(!*source)return PXA_STATUS_OK;
    pxa_status_t status=pxa_esp_volumes_release((*source)->epoch);
    if(status==PXA_STATUS_OK) {heap_caps_free(*source);*source=NULL;}
    return status;
}

#if defined(CONFIG_LV_USE_FREETYPE) && CONFIG_LV_USE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include <errno.h>
#define FONT_CACHE_PAGE_BYTES 4096u
typedef struct {
    uint64_t epoch;
    uint32_t size,position;
    int fd;
    uint8_t *cache;
    uint32_t cache_start[2];
    uint16_t cache_size[2];
    uint8_t recent;
} font_stream_t;
/* The source owner pins live font objects, while each open stream owns an
 * independent pin until FT_Done_Face closes it. No whole-font RAM copy. */
static unsigned long stream_read_direct(font_stream_t *file,unsigned long offset,
    unsigned char *buffer,unsigned long count) {
    if(offset!=file->position) {
        if(lseek(file->fd,(off_t)offset,SEEK_SET)!=(off_t)offset)return 0;
        file->position=(uint32_t)offset;
    }
    unsigned long done=0;
    while(done<count) {
        if(ready(file->epoch)!=PXA_STATUS_OK)break;
        size_t bytes=count-done;if(bytes>4096)bytes=4096;
        ssize_t n=read(file->fd,buffer+done,bytes);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        done+=(unsigned long)n;file->position+=(uint32_t)n;
    }
    return done;
}
static unsigned long stream_read(FT_Stream stream,unsigned long offset,
    unsigned char *buffer,unsigned long count) {
    font_stream_t *file=stream->descriptor.pointer;
    if(!file || ready(file->epoch)!=PXA_STATUS_OK || offset>file->size)
        return count?0:1;
    if(!count)return 0;
    if(count>file->size-offset)count=file->size-offset;
    if(!file->cache)return stream_read_direct(file,offset,buffer,count);
    unsigned long done=0;
    while(done<count && ready(file->epoch)==PXA_STATUS_OK) {
        const uint32_t at=(uint32_t)(offset+done);
        unsigned slot;
        for(slot=0;slot<2;++slot)
            if(at>=file->cache_start[slot] &&
               at-file->cache_start[slot]<file->cache_size[slot])break;
        if(slot==2) {
            slot=1u-file->recent;
            const uint32_t start=at&~(FONT_CACHE_PAGE_BYTES-1u);
            unsigned long bytes=file->size-start;
            if(bytes>FONT_CACHE_PAGE_BYTES)bytes=FONT_CACHE_PAGE_BYTES;
            file->cache_size[slot]=0;
            const unsigned long loaded=stream_read_direct(file,start,
                file->cache+slot*FONT_CACHE_PAGE_BYTES,bytes);
            if(loaded<=at-start)break;
            file->cache_start[slot]=start;
            file->cache_size[slot]=(uint16_t)loaded;
        }
        file->recent=(uint8_t)slot;
        const size_t begin=at-file->cache_start[slot];
        size_t bytes=file->cache_size[slot]-begin;
        if(bytes>count-done)bytes=count-done;
        memcpy(buffer+done,file->cache+slot*FONT_CACHE_PAGE_BYTES+begin,bytes);
        done+=bytes;
    }
    return done;
}
static void stream_close(FT_Stream stream) {
    font_stream_t *file=stream->descriptor.pointer;if(!file)return;
    (void)close(file->fd);(void)pxa_esp_volumes_release(file->epoch);
    heap_caps_free(file->cache);
    heap_caps_free(file);stream->descriptor.pointer=NULL;
}
FT_Error __real_FT_Stream_Open(FT_Stream,const char *);
FT_Error __wrap_FT_Stream_Open(FT_Stream stream,const char *name) {
    if(!stream || !name)return FT_Err_Invalid_Stream_Handle;
    if(strncmp(name,FONT_PREFIX,FONT_PREFIX_BYTES))return __real_FT_Stream_Open(stream,name);
    const char *digits=name+FONT_PREFIX_BYTES;uint64_t epoch=0;
    for(unsigned i=0;i<16;++i) {
        unsigned char c=(unsigned char)digits[i];unsigned value;
        if(c>='0' && c<='9')value=c-'0';
        else if(c>='a' && c<='f')value=c-'a'+10;
        else return FT_Err_Cannot_Open_Resource;
        epoch=(epoch<<4)|value;
    }
    if(!epoch || digits[16]!=':' || digits[17]!='/')return FT_Err_Cannot_Open_Resource;
    const char *path=digits+17;size_t length=strnlen(path,FONT_PATH_BYTES);
    if(length<2 || length>=FONT_PATH_BYTES || !pxa_fs_scoped_path_is_valid(
        (pxa_bytes_t){(const uint8_t *)path+1,length-1}))return FT_Err_Cannot_Open_Resource;
    if(pxa_esp_volumes_retain(epoch)!=PXA_STATUS_OK)return FT_Err_Cannot_Open_Resource;
    pxa_volume_info_t volume;pxa_status_t status=pxa_esp_storage_resolve_volume(path,&volume);
    int fd=-1;struct stat metadata;
    if(status==PXA_STATUS_OK && volume.mount_epoch==epoch && ready(epoch)==PXA_STATUS_OK)
        fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0 || fstat(fd,&metadata)!=0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink>1 ||
        metadata.st_size<=0 || (uint64_t)metadata.st_size>INT32_MAX || ready(epoch)!=PXA_STATUS_OK) {
        if(fd>=0)(void)close(fd);
        (void)pxa_esp_volumes_release(epoch);return FT_Err_Cannot_Open_Resource;
    }
    font_stream_t *file=font_allocate(sizeof(*file));
    if(!file) {(void)close(fd);(void)pxa_esp_volumes_release(epoch);return FT_Err_Out_Of_Memory;}
    *file=(font_stream_t){.epoch=epoch,.size=(uint32_t)metadata.st_size,.fd=fd};
    /* Optional two-page cache: keep table and glyph reads local without
     * copying a whole font. Epoch checks also apply to cache hits. */
    file->cache=font_allocate(2u*FONT_CACHE_PAGE_BYTES);
    stream->descriptor.pointer=file;stream->pathname.pointer=(char *)name;
    stream->size=file->size;stream->pos=0;stream->base=NULL;
    stream->read=stream_read;stream->close=stream_close;
    return FT_Err_Ok;
}
#endif
