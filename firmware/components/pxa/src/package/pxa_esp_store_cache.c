#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pxa_esp_store_cache.h"
#include "pxa_esp_record_seal.h"
#include "pxa_esp_volumes.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "pxa/wire.h"
#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(ESP_PLATFORM) && !defined(__linux__) && !defined(__APPLE__)
#include "pxa/esp/pxa_esp_posix_shim.h"
#define open(...) pxa_esp_open(__VA_ARGS__)
#define fsync(fd) pxa_esp_fsync(fd)
#define close(fd) pxa_esp_close(fd)
#endif

/* Wire fields, no native struct/padding. Authentication grants cache ownership
 * only; code always passes full publisher/container verification at install. */
#define BODY_BYTES (8u+20u+65u+130u+8u+32u+32u)
static const uint8_t purpose[]="PXA.store-cache.v2";
static int sync_parent(char *path) {
    char *slash=strrchr(path,'/');if(!slash)return -1;
    *slash=0;int descriptor=open(path,O_RDONLY|O_DIRECTORY);*slash='/';
    if(descriptor<0)return -1;
    int result=fsync(descriptor);if(close(descriptor))result=-1;return result;
}
/* Startup only, before workers exist. A pack lacking its completion record
 * is an interrupted Host transfer. Keep complete/unknown descriptors so an
 * unavailable key or a full RAM view never destroys an offline download. */
static void clean_incomplete(const char *directory) {
    DIR *dir=opendir(directory);struct dirent *entry;
    while(dir && (entry=readdir(dir))) {
        char path[384],metadata[384];struct stat st;
        if(!pxa_esp_store_cache_name_valid(entry->d_name))continue;
        if(snprintf(path,sizeof(path),"%s/%.19s",directory,entry->d_name)>=(int)sizeof(path) ||
            snprintf(metadata,sizeof(metadata),"%s.meta",path)>=(int)sizeof(metadata))continue;
        if(stat(metadata,&st)==0 || errno!=ENOENT || stat(path,&st) || !S_ISREG(st.st_mode))continue;
        if(!unlink(path)) {closedir(dir);dir=opendir(directory);}
    }
    if(dir)closedir(dir);
}
int pxa_esp_store_cache_name_valid(const char *name) {
    if(!name || strlen(name)!=19 || strncmp(name,".store-",7) || strcmp(name+15,".pxa"))return 0;
    for(size_t i=7;i<15;++i)if(!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f')))return 0;
    return 1;
}
pxa_status_t pxa_esp_store_cache_path(uint64_t epoch,const char *name,char *path,size_t capacity) {
    char mount[PXA_ESP_STORAGE_PATH_BYTES];
    if(!path || !capacity || !pxa_esp_store_cache_name_valid(name))return PXA_STATUS_INVALID_ARGUMENT;
    path[0]=0;pxa_status_t status=pxa_esp_storage_volume_path(epoch,mount,sizeof(mount));
    if(status!=PXA_STATUS_OK)return status;
    int n=snprintf(path,capacity,"%s/PXA/cache/store/%s",mount,name);
    return n<0 || (size_t)n>=capacity?PXA_STATUS_LIMIT_EXCEEDED:PXA_STATUS_OK;
}
static pxa_status_t descriptor_path(const pxa_esp_store_download_entry_t *entry,char *out,size_t capacity,const char *suffix) {
    pxa_status_t status=pxa_esp_store_cache_path(entry->epoch,entry->filename,out,capacity);
    if(status!=PXA_STATUS_OK)return status;
    size_t length=strlen(out),extra=strlen(suffix);
    if(length+extra>=capacity)return PXA_STATUS_LIMIT_EXCEEDED;
    memcpy(out+length,suffix,extra+1);return PXA_STATUS_OK;
}
static pxa_status_t tag(uint64_t epoch,uint8_t *wire,int verify) {
    pxa_volume_info_t volume;pxa_status_t status=pxa_esp_storage_volume(epoch,&volume);
    if(status!=PXA_STATUS_OK)return status;
    pxa_bytes_t p={purpose,sizeof(purpose)-1},binding={volume.id.bytes,sizeof(volume.id.bytes)},body={wire,BODY_BYTES};
    return verify?pxa_esp_record_verify(p,binding,body,wire+BODY_BYTES):
        pxa_esp_record_seal(p,binding,body,wire+BODY_BYTES);
}
pxa_status_t pxa_esp_store_cache_save(const pxa_esp_store_download_entry_t *entry) {
    uint8_t wire[BODY_BYTES+32]={0};char path[384]={0},temporary[384]={0};
    if(!entry || !entry->size || !memchr(entry->owner,0,sizeof(entry->owner)) || !entry->owner[0] ||
        !memchr(entry->app_id,0,sizeof(entry->app_id)) || !entry->app_id[0])return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_volumes_retain(entry->epoch);if(status!=PXA_STATUS_OK)return status;
    status=descriptor_path(entry,path,sizeof(path),".meta");
    if(status==PXA_STATUS_OK)status=descriptor_path(entry,temporary,sizeof(temporary),".meta.tmp");
    memcpy(wire,"PXADL2\0",8);memcpy(wire+8,entry->filename,20);memcpy(wire+28,entry->app_id,65);
    memcpy(wire+93,entry->owner,130);pxa_write_u64(wire+223,entry->size);
    memcpy(wire+231,entry->digest,32);memcpy(wire+263,entry->publisher,32);
    if(status==PXA_STATUS_OK)status=tag(entry->epoch,wire,0);
    FILE *file=NULL;
    if(status==PXA_STATUS_OK) {
        file=fopen(temporary,"wb");
        if(!file || fwrite(wire,1,sizeof(wire),file)!=sizeof(wire) || fflush(file) || fsync(fileno(file)))status=PXA_STATUS_IO_ERROR;
    }
    if(file && fclose(file))status=PXA_STATUS_IO_ERROR;
    if(status==PXA_STATUS_OK && rename(temporary,path))status=PXA_STATUS_IO_ERROR;
    if(status==PXA_STATUS_OK && sync_parent(path))status=PXA_STATUS_IO_ERROR;
    if(status!=PXA_STATUS_OK && temporary[0])unlink(temporary);
    pxa_esp_volumes_release(entry->epoch);return status;
}
pxa_status_t pxa_esp_store_cache_remove(const pxa_esp_store_download_entry_t *entry) {
    char path[384],descriptor[384];
    if(!entry)return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_volumes_retain(entry->epoch);if(status!=PXA_STATUS_OK)return status;
    status=descriptor_path(entry,descriptor,sizeof(descriptor),".meta");
    if(status==PXA_STATUS_OK)status=pxa_esp_store_cache_path(entry->epoch,entry->filename,path,sizeof(path));
    if(status==PXA_STATUS_OK && unlink(descriptor) && errno!=ENOENT)status=PXA_STATUS_IO_ERROR;
    if(status==PXA_STATUS_OK && unlink(path) && errno!=ENOENT)status=PXA_STATUS_IO_ERROR;
    if(status==PXA_STATUS_OK && sync_parent(path))status=PXA_STATUS_IO_ERROR;
    pxa_esp_volumes_release(entry->epoch);return status;
}
static void load_cache(pxa_esp_store_download_entry_t *out,size_t capacity,uint8_t cleanup) {
    pxa_volume_info_t volumes[PXA_ESP_STORAGE_MAX_VOLUMES];size_t used=0;
    if(out && capacity)memset(out,0,capacity*sizeof(*out));
    else if(!cleanup)return;
    size_t count=pxa_esp_storage_volumes(volumes,PXA_ESP_STORAGE_MAX_VOLUMES);
    for(size_t i=0;i<count && i<PXA_ESP_STORAGE_MAX_VOLUMES && (!out || used<capacity);++i) {
        uint64_t epoch=volumes[i].mount_epoch;char mount[PXA_ESP_STORAGE_PATH_BYTES],directory[320];
        if(pxa_esp_volumes_retain(epoch)!=PXA_STATUS_OK)continue;
        if(pxa_esp_storage_volume_path(epoch,mount,sizeof(mount))!=PXA_STATUS_OK) {pxa_esp_volumes_release(epoch);continue;}
        snprintf(directory,sizeof(directory),"%s/PXA/cache/store",mount);
        if(cleanup && (volumes[i].capabilities&PXA_VOLUME_WRITE))clean_incomplete(directory);
        if(!out) {pxa_esp_volumes_release(epoch);continue;}
        DIR *dir=opendir(directory);struct dirent *de;
        while(dir && used<capacity && (de=readdir(dir))) {
            char path[384],name[20];uint8_t wire[BODY_BYTES+32];struct stat st;
            if(strlen(de->d_name)!=24 || strcmp(de->d_name+19,".meta"))continue;
            memcpy(name,de->d_name,19);name[19]=0;if(!pxa_esp_store_cache_name_valid(name))continue;
            snprintf(path,sizeof(path),"%s/%.24s",directory,de->d_name);FILE *file=fopen(path,"rb");
            int valid=file && fread(wire,1,sizeof(wire),file)==sizeof(wire) && fgetc(file)==EOF;
            if(file)fclose(file);
            if(!valid || memcmp(wire,"PXADL2\0",8) || tag(epoch,wire,1)!=PXA_STATUS_OK ||
                memcmp(wire+8,name,20) || !memchr(wire+28,0,65) || !wire[28] || !memchr(wire+93,0,130) || !wire[93])continue;
            if(pxa_esp_store_cache_path(epoch,name,path,sizeof(path))!=PXA_STATUS_OK || stat(path,&st) ||
                !S_ISREG(st.st_mode) || st.st_size<=0 || (uint64_t)st.st_size!=pxa_read_u64(wire+223))continue;
            /* A 32-bit compatibility token collision must not select whichever
             * removable device happened to be enumerated first. */
            int duplicate=0;for(size_t j=0;j<used;++j)if(!strcmp(out[j].filename,name)) {out[j].owner[0]=0;duplicate=1;}
            if(duplicate)continue;
            pxa_esp_store_download_entry_t *entry=&out[used++];entry->epoch=epoch;entry->size=pxa_read_u64(wire+223);
            memcpy(entry->filename,name,20);memcpy(entry->app_id,wire+28,65);memcpy(entry->owner,wire+93,130);
            memcpy(entry->digest,wire+231,32);memcpy(entry->publisher,wire+263,32);
        }
        if(dir)closedir(dir);
        pxa_esp_volumes_release(epoch);
    }
}
void pxa_esp_store_cache_load(pxa_esp_store_download_entry_t *out,size_t capacity) {load_cache(out,capacity,0);}
void pxa_esp_store_cache_cleanup(void) {load_cache(NULL,0,1);}

pxa_status_t pxa_esp_store_cache_adopt(const char *source,const char *owner,char *token,size_t capacity) {
    pxa_volume_info_t volume;pxa_esp_store_download_entry_t entry={0};struct stat st;
    char expected[384];const char *name;uint8_t *buffer=NULL;FILE *file=NULL;
    pxa_esp_package_preview_t *preview=NULL;mbedtls_md_context_t hash;uint64_t bytes=0;
    if(!source || !owner || !*owner || strlen(owner)>=sizeof(entry.owner) || !token || capacity<20)return PXA_STATUS_INVALID_ARGUMENT;
    token[0]=0;name=strrchr(source,'/');if(!name || !pxa_esp_store_cache_name_valid(++name))return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status=pxa_esp_storage_resolve_volume(source,&volume);if(status!=PXA_STATUS_OK)return status;
    status=pxa_esp_store_cache_path(volume.mount_epoch,name,expected,sizeof(expected));
    if(status!=PXA_STATUS_OK || strcmp(source,expected))return PXA_STATUS_DENIED;
    status=pxa_esp_volumes_retain(volume.mount_epoch);if(status!=PXA_STATUS_OK)return status;
    mbedtls_md_init(&hash);
    if(stat(source,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || (uint64_t)st.st_size>UINT64_C(0x7fffffff)) {
        status=PXA_STATUS_INVALID_ARGUMENT;goto done;
    }
    preview=heap_caps_calloc(1,sizeof(*preview),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    buffer=heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!preview || !buffer) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    if(!pxa_esp_package_store_preview_file(source,preview)) {status=PXA_STATUS_DENIED;goto done;}
    file=fopen(source,"rb");
    if(!file || mbedtls_md_setup(&hash,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) || mbedtls_md_starts(&hash)) {
        status=PXA_STATUS_IO_ERROR;goto done;
    }
    for(;;) {
        size_t count=fread(buffer,1,16384,file);if(!count)break;
        if(bytes>(uint64_t)st.st_size || count>(uint64_t)st.st_size-bytes || mbedtls_md_update(&hash,buffer,count)) {
            status=PXA_STATUS_IO_ERROR;goto done;
        }
        bytes+=count;
    }
    if(ferror(file) || bytes!=(uint64_t)st.st_size || mbedtls_md_finish(&hash,entry.digest)) {status=PXA_STATUS_IO_ERROR;goto done;}
    memcpy(entry.filename,name,20);strcpy(entry.owner,owner);strcpy(entry.app_id,preview->app_id);
    memcpy(entry.publisher,preview->publisher_root,32);entry.size=bytes;entry.epoch=volume.mount_epoch;
    status=pxa_esp_store_cache_save(&entry);
    if(status==PXA_STATUS_OK)memcpy(token,name,20);
done:
    if(file)fclose(file);
    mbedtls_md_free(&hash);heap_caps_free(preview);heap_caps_free(buffer);
    pxa_esp_volumes_release(volume.mount_epoch);return status;
}
