#include "pxa/pxa_container_stream.h"
#include "pxa_esp_package_trust.h"
#include "pxa_esp_volumes.h"
#include "pxa/posix/pxa_posix_fs.h"
#include "esp_heap_caps.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct pxa_host_container_stream {
    pxa_esp_mbedtls_trust_t trust;
    pxa_posix_installer_t *installer;
    pxa_posix_container_stream_t *stream;
    void *workspace;
    char *directory;
    pxa_openssl_sha256_stream_t hash;
    uint64_t expected,received;
    uint64_t volume_epoch;
    uint8_t digest[32],has_digest,finished;
    pxa_status_t status;
};
int pxa_host_container_stream_volume_path(uint64_t epoch,const char *directory) {
    char prefix[PXA_ESP_STORAGE_PATH_BYTES+32];
    if(!epoch || !directory || pxa_esp_storage_volume_path(epoch,prefix,sizeof(prefix))!=PXA_STATUS_OK)return 0;
    size_t n=strlen(prefix);
    static const char cache[]="/PXA/cache/store/.stream-";
    if(n+sizeof(cache)>sizeof(prefix))return 0;
    memcpy(prefix+n,cache,sizeof(cache));n+=sizeof(cache)-1;
    if(strncmp(directory,prefix,n) || strlen(directory+n)!=8)return 0;
    for(size_t i=0;i<8;++i) {
        char c=directory[n+i];
        if(!((c>='0' && c<='9') || (c>='a' && c<='f')))return 0;
    }
    return 1;
}
pxa_status_t pxa_host_container_stream_begin_volume(uint64_t epoch,const char *directory,
    uint64_t bytes,const uint8_t sha256[32],pxa_host_container_stream_t **output) {
    if(!output || !sha256)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;
    pxa_status_t status=pxa_esp_volumes_retain(epoch);
    if(status!=PXA_STATUS_OK)return status;
    pxa_volume_info_t volume;
    status=pxa_esp_storage_volume(epoch,&volume);
    const uint64_t needed=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_INSTALLABLE;
    if(status==PXA_STATUS_OK && ((volume.capabilities&needed)!=needed ||
        !pxa_host_container_stream_volume_path(epoch,directory)))status=PXA_STATUS_DENIED;
    char path[PXA_ESP_STORAGE_PATH_BYTES+32];
    if(status==PXA_STATUS_OK)status=pxa_esp_storage_volume_path(epoch,path,sizeof(path));
    const char *parts[]={"/PXA","/cache","/store"};
    for(size_t i=0;status==PXA_STATUS_OK && i<3;++i) {
        size_t n=strlen(path),extra=strlen(parts[i]);struct stat info;
        if(n+extra>=sizeof(path)) {status=PXA_STATUS_LIMIT_EXCEEDED;break;}
        memcpy(path+n,parts[i],extra+1);
        if(mkdir(path,0700) && errno!=EEXIST)status=PXA_STATUS_IO_ERROR;
        else if(stat(path,&info) || !S_ISDIR(info.st_mode))status=PXA_STATUS_DENIED;
    }
    if(status==PXA_STATUS_OK)status=pxa_host_container_stream_begin(directory,bytes,sha256,output);
    if(status==PXA_STATUS_OK)(*output)->volume_epoch=epoch;
    else pxa_esp_volumes_release(epoch);
    return status;
}
pxa_status_t pxa_host_container_stream_begin(const char *directory,uint64_t bytes,
    const uint8_t *sha256,pxa_host_container_stream_t **output) {
    if(!output || !directory || bytes<64)return PXA_STATUS_INVALID_ARGUMENT;
    *output=NULL;
    pxa_host_container_stream_t *s=calloc(1,sizeof(*s));
    if(!s)return PXA_STATUS_RESOURCE_LIMIT;
    pxa_posix_installer_config_t config;
    s->status=pxa_esp_package_installer_profile(&config,&s->trust);
    s->directory=strdup(directory);
    if(!s->directory) {s->status=PXA_STATUS_RESOURCE_LIMIT;goto fail;}
    config.storage_root=s->directory;
    if(s->status!=PXA_STATUS_OK)goto fail;
    size_t size=pxa_posix_installer_workspace_size(&config);
    s->workspace=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s->workspace) {s->status=PXA_STATUS_RESOURCE_LIMIT;goto fail;}
    s->status=pxa_posix_installer_init(s->workspace,size,&config,&s->installer);
    if(s->status==PXA_STATUS_OK)s->status=pxa_openssl_sha256_stream_begin(&s->hash);
    if(s->status==PXA_STATUS_OK)s->status=pxa_posix_container_stream_begin(s->installer,directory,&s->stream);
    if(s->status!=PXA_STATUS_OK)goto fail;
    s->expected=bytes;s->has_digest=sha256!=NULL;
    if(sha256)memcpy(s->digest,sha256,32);
    *output=s;return PXA_STATUS_OK;
fail:;
    pxa_status_t status=s->status;pxa_host_container_stream_close(s);return status;
}
pxa_status_t pxa_host_container_stream_feed(pxa_host_container_stream_t *s,const uint8_t *data,size_t size) {
    if(!s)return PXA_STATUS_INVALID_ARGUMENT;
    if(s->status!=PXA_STATUS_OK)return s->status;
    if(s->finished)return PXA_STATUS_BAD_STATE;
    if(s->volume_epoch) {
        pxa_volume_info_t volume;
        s->status=pxa_esp_storage_volume(s->volume_epoch,&volume);
        if(s->status==PXA_STATUS_OK && (volume.state!=PXA_VOLUME_READY || volume.ambiguous))s->status=PXA_STATUS_UNAVAILABLE;
        if(s->status!=PXA_STATUS_OK)return s->status;
    }
    if(size>s->expected-s->received)return s->status=PXA_STATUS_DENIED;
    s->status=pxa_openssl_sha256_stream_update(&s->hash,data,size);
    if(s->status==PXA_STATUS_OK)s->status=pxa_posix_container_stream_feed(s->stream,data,size);
    if(s->status==PXA_STATUS_OK)s->received+=size;
    return s->status;
}
pxa_status_t pxa_host_container_stream_finish(pxa_host_container_stream_t *s) {
    uint8_t digest[32];
    if(!s)return PXA_STATUS_INVALID_ARGUMENT;
    if(s->status!=PXA_STATUS_OK)return s->status;
    if(s->finished)return PXA_STATUS_OK;
    if(s->received!=s->expected)return s->status=PXA_STATUS_DENIED;
    s->status=pxa_openssl_sha256_stream_finish(&s->hash,digest);
    if(s->status==PXA_STATUS_OK && s->has_digest && memcmp(digest,s->digest,32))s->status=PXA_STATUS_DENIED;
    if(s->status==PXA_STATUS_OK)s->status=pxa_posix_container_stream_finish(s->stream);
    if(s->status==PXA_STATUS_OK) {
        s->finished=1;
        pxa_posix_container_stream_close(s->stream);s->stream=NULL;
        pxa_posix_installer_deinit(s->installer);s->installer=NULL;
        heap_caps_free(s->workspace);s->workspace=NULL;
    }
    return s->status;
}
void pxa_host_container_stream_close(pxa_host_container_stream_t *s) {
    if(!s)return;
    pxa_posix_container_stream_close(s->stream);
    pxa_openssl_sha256_stream_abort(&s->hash);
    pxa_posix_installer_deinit(s->installer);
    if(s->volume_epoch) {
        (void)pxa_posix_fs_remove_tree(s->directory);
        pxa_esp_volumes_release(s->volume_epoch);
    }
    heap_caps_free(s->workspace);free(s->directory);free(s);
}
