#include "pxa_esp_store_download.h"
#include "pxa_esp_package_limits.h"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "pxa_esp_store_cache.h"
#include "pxa_esp_volumes.h"
#include "pxa/container.h"
#include "mbedtls/md.h"
#include "pxa/pxa_host.h"
#include "pxa/status.h"

#ifndef CONFIG_PXA_STORE_ORIGIN
#define CONFIG_PXA_STORE_ORIGIN "https://app.doit.am"
#endif

typedef struct {
    FILE *file;
    pxa_esp_store_job_t *job;
    mbedtls_md_context_t sha;
    uint64_t received;
    uint64_t expected;
    uint64_t progress_base;
    uint8_t hash_enabled;
    int failed;
} download_sink_t;

int pxa_esp_store_job_decode_identity(pxa_esp_store_job_t *job,const uint8_t *payload,size_t size) {
    if(!job || !payload || size<32 || !pxa_esp_store_job_decode(job,payload+32,size-32))return 0;
    memcpy(job->expected_root,payload,32);job->has_expected_root=1;return 1;
}
int pxa_esp_store_job_decode(pxa_esp_store_job_t *job,
                             const uint8_t *payload, size_t size) {
    static const char prefix[] = "/api/v2/artifacts/";
    static const char suffix[] = "/download?expires=";
    static const char signature[] = "&signature=";
    size_t offset;
    size_t app_size;
    size_t ticket_size;
    if (job == NULL || payload == NULL || size < 43u) return 0;
    job->has_expected_root=0;
    app_size = payload[0];
    ticket_size = (size_t)payload[1] | ((size_t)payload[2] << 8);
    if (app_size == 0 || app_size > 64 || ticket_size >= sizeof(job->ticket) ||
        size != 43u + app_size + ticket_size) return 0;
    job->size = 0;
    for (size_t index = 0; index < 8u; ++index)
        job->size |= (uint64_t)payload[3u + index] << (index * 8u);
    if (job->size == 0 || job->size > PXA_STORE_DOWNLOAD_MAX_BYTES) return 0;
    memcpy(job->digest, payload + 11u, 32u);
    memcpy(job->app_id, payload + 43u, app_size);
    job->app_id[app_size] = '\0';
    for (size_t index = 0; index < app_size; ++index) {
        const char byte = job->app_id[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
              byte == '.' || byte == '_' || byte == '-')) return 0;
    }
    memcpy(job->ticket, payload + 43u + app_size, ticket_size);
    job->ticket[ticket_size] = '\0';
    if (memcmp(job->ticket, prefix, sizeof(prefix) - 1u) != 0) return 0;
    offset = sizeof(prefix) - 1u;
    if (job->ticket[offset] < '0' || job->ticket[offset] > '9') return 0;
    while (job->ticket[offset] >= '0' && job->ticket[offset] <= '9') ++offset;
    if (strncmp(job->ticket + offset, suffix, sizeof(suffix) - 1u) != 0) return 0;
    offset += sizeof(suffix) - 1u;
    if (job->ticket[offset] < '0' || job->ticket[offset] > '9') return 0;
    while (job->ticket[offset] >= '0' && job->ticket[offset] <= '9') ++offset;
    if (strncmp(job->ticket + offset, signature, sizeof(signature) - 1u) != 0)
        return 0;
    offset += sizeof(signature) - 1u;
    if (ticket_size - offset != 64u) return 0;
    for (; offset < ticket_size; ++offset) {
        const char byte = job->ticket[offset];
        if (!((byte >= 'a' && byte <= 'f') || (byte >= '0' && byte <= '9')))
            return 0;
    }
    return 1;
}

static esp_err_t download_event(esp_http_client_event_t *event) {
    download_sink_t *sink = (download_sink_t *)event->user_data;
    size_t length;
    /* Stop writes immediately. ESP-IDF may keep draining the response even
     * when an event returns failure; the worker owns close/unlink after perform
     * returns (or its network timeout), never another task racing this FILE. */
    if (sink->job != NULL && __atomic_load_n(&sink->job->cancelled, __ATOMIC_ACQUIRE)) {
        sink->failed = 1;
        return ESP_FAIL;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
        return ESP_OK;
    length = (size_t)event->data_len;
    if (sink->failed || length > sink->expected - sink->received ||
        fwrite(event->data, 1, length, sink->file) != length ||
        (sink->hash_enabled && mbedtls_md_update(&sink->sha, event->data, length) != 0)) {
        sink->failed = 1;
        return ESP_FAIL;
    }
    sink->received += length;
    if (sink->job != NULL && sink->expected != 0u) {
        uint64_t total=sink->job->total_bytes?sink->job->total_bytes:sink->expected;
        uint8_t percent = (uint8_t)((sink->progress_base+sink->received) * 100u / total);
        if (percent >= sink->job->reported_percent + 5u ||
            sink->received == sink->expected) {
            sink->job->downloaded_bytes = sink->progress_base+sink->received;
            sink->job->reported_percent = percent;
            sink->job->notify(sink->job, 4u);
        }
    }
    return ESP_OK;
}

/* Read at most the signed metadata prefix. A server cannot turn preflight
 * into an unbounded allocation or a full container download. */
#define METADATA_MAX (PXA_ESP_SIGNED_MANIFEST_MAX_BYTES+PXA_CONTAINER_HEADER_BYTES+216u)
static int cancelled(const pxa_esp_store_job_t *job) {
    return __atomic_load_n(&job->cancelled,__ATOMIC_ACQUIRE);
}
static __attribute__((noinline)) pxa_status_t read_metadata(pxa_esp_store_job_t *job,const char *url,
    uint64_t expected_bytes,uint8_t cached,uint8_t *prefix,size_t *bytes) {
    FILE *file=NULL;esp_http_client_handle_t client=NULL;pxa_container_header_t header;
    esp_http_client_config_t config={.method=HTTP_METHOD_GET,.timeout_ms=30000,
        .disable_auto_redirect=true,.crt_bundle_attach=esp_crt_bundle_attach,.buffer_size=1024};
    pxa_status_t status=PXA_STATUS_IO_ERROR;size_t have=0,need=PXA_CONTAINER_HEADER_BYTES;
    char metadata_url[512];
    if(cached)file=fopen(job->path,"rb");
    else {
        const char *suffix=strstr(url,"/download?expires=");
        if(!suffix || snprintf(metadata_url,sizeof(metadata_url),"%.*s/install-metadata%s",
            (int)(suffix-url),url,suffix+9)>=(int)sizeof(metadata_url))return PXA_STATUS_INVALID_ARGUMENT;
        config.url=metadata_url;client=esp_http_client_init(&config);
        if(!client || esp_http_client_open(client,0)!=ESP_OK)goto done;
        int64_t length=esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client)==404) {
            /* Older stores can serve the same bounded prefix from the
             * container. Read only that prefix, then close the response. */
            esp_http_client_cleanup(client);config.url=url;client=esp_http_client_init(&config);
            /* The metadata URL is no longer needed; reuse its buffer for the
             * bounded fallback range instead of growing this worker frame. */
            (void)snprintf(metadata_url,sizeof(metadata_url),"bytes=0-%u",(unsigned)(METADATA_MAX-1u));
            if(!client || esp_http_client_set_header(client,"Range",metadata_url)!=ESP_OK ||
                esp_http_client_open(client,0)!=ESP_OK)goto done;
            (void)esp_http_client_fetch_headers(client);
            int code=esp_http_client_get_status_code(client);
            if(code!=200 && code!=206)goto done;
        } else if(esp_http_client_get_status_code(client)!=200 || length>(int64_t)METADATA_MAX)goto done;
    }
    if(!file && !client)goto done;
    for(;;) {
        if(cancelled(job)) {status=PXA_STATUS_CANCELLED;goto done;}
        while(have<need) {
            int got=file?(int)fread(prefix+have,1,need-have,file):esp_http_client_read(client,(char *)prefix+have,(int)(need-have));
            if(got<=0) {status=PXA_STATUS_IO_ERROR;goto done;}
            have+=(size_t)got;
            if(cancelled(job)) {status=PXA_STATUS_CANCELLED;goto done;}
        }
        if(need!=PXA_CONTAINER_HEADER_BYTES)break;
        uint64_t declared=expected_bytes?expected_bytes:pxa_read_u64(prefix+48);
        if(!declared || declared>PXA_STORE_DOWNLOAD_MAX_BYTES) {status=PXA_STATUS_LIMIT_EXCEEDED;goto done;}
        status=pxa_container_header_parse((pxa_bytes_t){prefix,have},declared,&header);
        if(status!=PXA_STATUS_OK)goto done;
        if(header.payload_offset>METADATA_MAX || header.payload_offset<=have) {status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
        need=(size_t)header.payload_offset;
    }
    *bytes=have;status=PXA_STATUS_OK;
 done:
    if(file)fclose(file);
    if(client)esp_http_client_cleanup(client);
    return status;
}
static __attribute__((noinline)) pxa_status_t create_cache_directory(uint64_t epoch) {
    char path[384];struct stat st;
    pxa_status_t status=pxa_esp_storage_volume_path(epoch,path,sizeof(path));if(status!=PXA_STATUS_OK)return status;
    const char *parts[]={"/PXA","/cache","/store"};
    for(size_t i=0;i<3;++i) {
        if(strlen(path)+strlen(parts[i])>=sizeof(path))return PXA_STATUS_LIMIT_EXCEEDED;
        strcat(path,parts[i]);
        if(mkdir(path,0700) && errno!=EEXIST)return PXA_STATUS_IO_ERROR;
        if(stat(path,&st) || !S_ISDIR(st.st_mode))return PXA_STATUS_BAD_STATE;
    }
    return PXA_STATUS_OK;
}
/* Close transport and crypto before entering the filesystem coordinator.
 * This separate frame is also retired before the SDK/FAT call depth. */
static __attribute__((noinline)) pxa_status_t download_file(pxa_esp_store_job_t *job,const char *url,
    uint64_t epoch,uint64_t bytes,const uint8_t *expected_digest,uint64_t progress_base,
    char *path,size_t path_bytes,char *filename,size_t filename_bytes) {
    download_sink_t sink={0};esp_http_client_handle_t client=NULL;int descriptor=-1,owned=0;
    uint8_t digest[32];pxa_status_t status=create_cache_directory(epoch);
    path[0]=0;mbedtls_md_init(&sink.sha);
    if(status!=PXA_STATUS_OK)goto done;
    for(unsigned attempt=0;attempt<8;++attempt) {
        snprintf(filename,filename_bytes,".store-%08lx.pxa",(unsigned long)esp_random());
        status=pxa_esp_store_cache_path(epoch,filename,path,path_bytes);
        if(status!=PXA_STATUS_OK)goto done;
        descriptor=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);
        if(descriptor>=0 || errno!=EEXIST)break;
    }
    if(descriptor<0) {path[0]=0;status=PXA_STATUS_IO_ERROR;goto done;}
    owned=1;sink.file=fdopen(descriptor,"wb");
    if(!sink.file) {status=PXA_STATUS_IO_ERROR;goto done;}descriptor=-1;
    sink.expected=bytes;sink.job=job;sink.progress_base=progress_base;sink.hash_enabled=expected_digest!=NULL;
    if(sink.hash_enabled && (mbedtls_md_setup(&sink.sha,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0) || mbedtls_md_starts(&sink.sha))) {
        status=PXA_STATUS_RESOURCE_LIMIT;goto done;
    }
    esp_http_client_config_t config={.url=url,.method=HTTP_METHOD_GET,.timeout_ms=30000,
        .disable_auto_redirect=true,.crt_bundle_attach=esp_crt_bundle_attach,.event_handler=download_event,
        .user_data=&sink,.buffer_size=1024};
    client=esp_http_client_init(&config);
    esp_err_t fetched=client?esp_http_client_perform(client):ESP_FAIL;
    status=PXA_STATUS_IO_ERROR;
    if(cancelled(job)) {status=PXA_STATUS_CANCELLED;goto done;}
    if(!client || fetched!=ESP_OK || esp_http_client_get_status_code(client)!=200 || sink.failed ||
        sink.received!=bytes || (sink.hash_enabled && (mbedtls_md_finish(&sink.sha,digest) || memcmp(digest,expected_digest,32))) ||
        fflush(sink.file) || fsync(fileno(sink.file)))goto done;
    int closed=fclose(sink.file);sink.file=NULL;
    if(closed)goto done;
    status=PXA_STATUS_OK;
done:
    if(client)esp_http_client_cleanup(client);
    mbedtls_md_free(&sink.sha);
    if(sink.file)fclose(sink.file);
    if(descriptor>=0)close(descriptor);
    if(status!=PXA_STATUS_OK && owned) {unlink(path);path[0]=0;}
    return status;
}
void pxa_esp_store_worker(void *context) {
    pxa_esp_store_job_t *job=context;
    uint8_t *prefix=NULL;size_t prefix_bytes=0;char url[512]={0},resource_filename[24];
    int attempted_install=0,owned_file=0;uint64_t retained_epoch=0,retained_media_epoch=0;
    const char *origin=CONFIG_PXA_STORE_ORIGIN;size_t origin_size=strlen(origin);
    job->status=PXA_STATUS_IO_ERROR;job->resource_path[0]=0;job->total_bytes=job->size;
    job->keep_file=job->separate==2;
    if(cancelled(job)) {job->status=PXA_STATUS_CANCELLED;goto done;}
    if(job->separate==2) {
        job->keep_file=1;job->status=pxa_esp_volumes_retain(job->cache_epoch);
        if(job->status!=PXA_STATUS_OK)goto done;
        retained_epoch=job->cache_epoch;
    } else if(origin_size<9 || strncmp(origin,"https://",8) || strchr(origin+8,'/') || strchr(origin+8,'?') ||
        snprintf(url,sizeof(url),"%s%s",origin,job->ticket)>=(int)sizeof(url)) {
        job->status=PXA_STATUS_INVALID_ARGUMENT;goto done;
    }
    prefix=heap_caps_malloc(METADATA_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!prefix) {job->status=PXA_STATUS_RESOURCE_LIMIT;goto done;}
    job->status=read_metadata(job,url,job->size,job->separate==2,prefix,&prefix_bytes);
    if(job->status!=PXA_STATUS_OK)goto done;
    job->status=pxa_esp_package_store_prepare_install(prefix,prefix_bytes,job->size,
        job->separate==2?0:job->size,job->separate==1,job->has_expected_root?job->expected_root:NULL,job->app_id,&job->preview,&job->placement);
    if(job->status!=PXA_STATUS_OK)goto done;
    if(job->placement.requires_resources) {
        if(job->separate==1) {job->status=PXA_STATUS_UNSUPPORTED;goto done;}
        if(job->separate==2)job->status=pxa_esp_package_store_prepare_resources(&job->placement,NULL,0,0,0);
        else {
            const char *tail=strstr(url,"/download?expires=");
            if(!tail || snprintf(job->resource_url,sizeof(job->resource_url),"%.*s/resources%s",
                (int)(tail-url),url,tail)>=(int)sizeof(job->resource_url)) {job->status=PXA_STATUS_INVALID_ARGUMENT;goto done;}
            job->status=read_metadata(job,job->resource_url,0,0,prefix,&prefix_bytes);
            if(job->status==PXA_STATUS_OK) {
                uint64_t bytes=pxa_read_u64(prefix+48);
                job->status=pxa_esp_package_store_prepare_resources(&job->placement,prefix,prefix_bytes,bytes,bytes);
            }
        }
        if(job->status!=PXA_STATUS_OK)goto done;
    }
    heap_caps_free(prefix);prefix=NULL;
    job->approved=0;job->notify(job,1);(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    if(cancelled(job)) {job->status=PXA_STATUS_CANCELLED;goto done;}
    if(!job->approved) {job->status=PXA_STATUS_DENIED;goto done;}
    job->status=pxa_esp_package_store_reserve_install(&job->placement,job->placement.selected);
    if(job->status!=PXA_STATUS_OK)goto done;
    if(job->separate!=2) {
        job->cache_epoch=job->placement.plan.app_mount_epoch;
        job->status=pxa_esp_volumes_retain(job->cache_epoch);if(job->status!=PXA_STATUS_OK)goto done;
        retained_epoch=job->cache_epoch;
    }
    if(job->placement.requires_resources) {
        uint64_t epoch=job->placement.media_choices[job->placement.media_selected].volume.mount_epoch;
        job->status=pxa_esp_volumes_retain(epoch);if(job->status!=PXA_STATUS_OK)goto done;
        retained_media_epoch=epoch;
        if(!job->placement.media_choices[job->placement.media_selected].reuse)
            job->total_bytes+=job->placement.resource_container_bytes;
    }
    if(job->separate!=2) {
        job->status=download_file(job,url,job->cache_epoch,job->size,job->digest,0,
            job->path,sizeof(job->path),job->filename,sizeof(job->filename));
        if(job->status!=PXA_STATUS_OK)goto done;
        owned_file=1;
    }
    if(cancelled(job)) {job->status=PXA_STATUS_CANCELLED;goto done;}
    if(job->placement.requires_resources && !job->placement.media_choices[job->placement.media_selected].reuse) {
        job->status=download_file(job,job->resource_url,retained_media_epoch,job->placement.resource_container_bytes,NULL,job->size,
            job->resource_path,sizeof(job->resource_path),resource_filename,sizeof(resource_filename));
        if(job->status!=PXA_STATUS_OK)goto done;
        if(cancelled(job)) {job->status=PXA_STATUS_CANCELLED;goto done;}
        /* The application signature binds the resource manifest, and the
         * SDK authenticates both resource signatures and every decoded file.
         * No unsigned HTTP header becomes a resource trust credential. */
        job->status=pxa_esp_package_store_commit_resources(job->resource_path,&job->placement);
        if(job->status!=PXA_STATUS_OK)goto done;
        if(unlink(job->resource_path)!=0) {job->status=PXA_STATUS_IO_ERROR;goto done;}
        job->resource_path[0]=0;
    }
    if(cancelled(job)) {job->status=PXA_STATUS_CANCELLED;goto done;}
    if(job->separate==1) {
        job->status=PXA_STATUS_OK;job->keep_file=1;
        job->notify(job,3);(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    } else {
        attempted_install=1;job->status=pxa_esp_package_store_commit_install(job->path,&job->placement);
        if(job->status==PXA_STATUS_OK)job->keep_file=0;
    }
done:
    heap_caps_free(prefix);
    if(job->resource_path[0])unlink(job->resource_path);
    if(!job->keep_file && job->path[0] && (owned_file || job->separate==2))unlink(job->path);
    if(job->placement.plan.reserved) {
        pxa_esp_storage_release_plan(&job->placement.plan);
        pxa_esp_storage_refresh_volume(job->placement.plan.app_mount_epoch);
        if(retained_media_epoch && retained_media_epoch!=job->placement.plan.app_mount_epoch)
            pxa_esp_storage_refresh_volume(retained_media_epoch);
    }
    if(retained_media_epoch)pxa_esp_volumes_release(retained_media_epoch);
    if(retained_epoch)pxa_esp_volumes_release(retained_epoch);
    ESP_LOGI("PxaStore","Transfer completed: status=%ld install=%d stack_spare=%u",(long)job->status,attempted_install,
        (unsigned)(uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t)));
    job->notify(job,2);vTaskDelete(NULL);
}
#endif
