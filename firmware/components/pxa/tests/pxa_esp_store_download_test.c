#define _POSIX_C_SOURCE 200809L
#define ESP_PLATFORM 1
#define CONFIG_PXA_STATE_ROOT "state"
#include "pxa_test_platform.h"
#include "pxa_esp_store_download.h"
#include "pxa_esp_store_cache.h"
#include "pxa/container.h"
#include "pxa/pxa_container_stream.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/md.h>

typedef uint32_t StackType_t;
static esp_http_client_config_t http;
static uint8_t *body;
static uint8_t *resource_body;
static size_t resource_size=3u*1024u*1024u;
static size_t body_size=5u*1024u*1024u,metadata_size=64+12+216,read_offset;
static unsigned random_id,completions,deletes,confirmations,installs,transfers,reservations,releases;
static bool approve=true,short_body,short_metadata,bad_metadata,low_space,reserve_fail,stale_record,cancel_prompt;
static bool is_metadata,old_server;
static bool http_live;
static bool with_resources,reuse_resources,bad_resources,resource_commit_fail;
static unsigned resource_commits,retained[4];
static bool resource_request;
static unsigned metadata_requests;
static unsigned started_progress;
static pxa_esp_store_job_t *waiting_job;
void *heap_caps_malloc(size_t n,unsigned caps) {assert(caps&MALLOC_CAP_SPIRAM);return malloc(n);}
void heap_caps_free(void *p) {free(p);}
void vTaskDelete(TaskHandle_t t) {(void)t;++deletes;}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t) {(void)t;return 512;}
uint32_t ulTaskNotifyTake(int clear,TickType_t ticks) {
    (void)clear;(void)ticks;assert(waiting_job);
    if(cancel_prompt)waiting_job->cancelled=1;
    waiting_job=NULL;return 1;
}
void test_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
uint32_t esp_random(void) {return ++random_id;}
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(!http_live);http_live=true;http=*config;resource_request=strstr(config->url,"/resources/")!=NULL;
    assert(config->disable_auto_redirect);return &http;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c,int n) {
    (void)c;(void)n;assert(strstr(http.url,"/install-metadata?expires=") || (old_server && strstr(http.url,"/download?expires=")));++metadata_requests;is_metadata=true;read_offset=0;return ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {(void)c;return (int64_t)metadata_size;}
int esp_http_client_read(esp_http_client_handle_t c,char *out,int n) {
    (void)c;assert(is_metadata);
    size_t limit=metadata_size-(short_metadata?1:0),got=(size_t)n;
    if(got>limit-read_offset)got=limit-read_offset;
    if(got>17)got=17;
    memcpy(out,(resource_request?resource_body:body)+read_offset,got);read_offset+=got;return (int)got;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    (void)c;++transfers;is_metadata=false;assert(strstr(http.url,"/download?expires="));
    esp_http_client_event_t e={.user_data=http.user_data,.event_id=HTTP_EVENT_ON_DATA};
    size_t remaining=(resource_request?resource_size:body_size)-(short_body?1:0),offset=0;
    while(remaining) {
        size_t n=remaining>4096?4096:remaining;e.data=(resource_request?resource_body:body)+offset;e.data_len=(int)n;
        if(http.event_handler(&e)!=ESP_OK)return ESP_FAIL;
        remaining-=n;offset+=n;
    }
    return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,const char *key,const char *value) {(void)c;assert(!strcmp(key,"Range") && !strcmp(value,"bytes=0-16663"));return ESP_OK;}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {(void)c;return old_server && is_metadata && strstr(http.url,"/install-metadata?")?404:200;}
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t c,int *error,int *flags) {
    (void)c;*error=0;*flags=0;return ESP_OK;
}
void esp_http_client_cleanup(esp_http_client_handle_t c) {(void)c;assert(http_live);http_live=false;}
pxa_status_t pxa_esp_volumes_retain(uint64_t epoch) {assert(epoch==2 || epoch==3);++retained[epoch];return PXA_STATUS_OK;}
pxa_status_t pxa_esp_volumes_release(uint64_t epoch) {assert((epoch==2 || epoch==3) && retained[epoch]);--retained[epoch];return PXA_STATUS_OK;}
pxa_status_t pxa_esp_storage_volume_path(uint64_t epoch,char *out,size_t bytes) {
    assert(epoch==2 || epoch==3);assert(snprintf(out,bytes,"%s%s",CONFIG_PXA_MOUNT_POINT,epoch==3?"/third":"")<(int)bytes);return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_storage_refresh_volume(uint64_t epoch) {assert(epoch==2 || epoch==3);return PXA_STATUS_OK;}
pxa_status_t pxa_esp_storage_release_plan(pxa_install_plan_t *plan) {
    assert(plan->reserved);plan->reserved=0;++releases;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_store_cache_path(uint64_t epoch,const char *name,char *out,size_t bytes) {
    assert(epoch==2 || epoch==3);assert(snprintf(out,bytes,"%s%s/PXA/cache/store/%s",CONFIG_PXA_MOUNT_POINT,epoch==3?"/third":"",name)<(int)bytes);return PXA_STATUS_OK;
}
/* The real parser/decoder is covered by SDK signed-container tests. These
 * transport doubles check routing, admission, cancellation and ownership. */
struct pxa_host_container_stream {char *path;size_t expected,received;uint8_t digest[32];int has_digest,finished;mbedtls_md_context_t hash;};
static unsigned stream_begins,stream_finishes,stream_closes;
static bool stream_reject;
pxa_status_t pxa_posix_fs_remove_tree(const char *path) {
    if(!rmdir(path))return PXA_STATUS_OK;
    return errno==ENOENT?PXA_STATUS_NOT_FOUND:PXA_STATUS_IO_ERROR;
}
pxa_status_t pxa_host_container_stream_begin(const char *path,uint64_t bytes,const uint8_t *digest,pxa_host_container_stream_t **out) {
    *out=NULL;assert(!mkdir(path,0700));
    pxa_host_container_stream_t *s=calloc(1,sizeof(*s));assert(s);
    s->path=strdup(path);s->expected=bytes;s->has_digest=digest!=NULL;if(digest)memcpy(s->digest,digest,32);
    mbedtls_md_init(&s->hash);assert(!mbedtls_md_setup(&s->hash,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),0));
    assert(!mbedtls_md_starts(&s->hash));*out=s;++stream_begins;return PXA_STATUS_OK;
}
pxa_status_t pxa_host_container_stream_feed(pxa_host_container_stream_t *s,const uint8_t *data,size_t size) {
    if(stream_reject)return PXA_STATUS_DENIED;
    assert(size<=s->expected-s->received);s->received+=size;
    assert(!mbedtls_md_update(&s->hash,data,size));return PXA_STATUS_OK;
}
pxa_status_t pxa_host_container_stream_finish(pxa_host_container_stream_t *s) {
    uint8_t digest[32];assert(s->received==s->expected);assert(!mbedtls_md_finish(&s->hash,digest));
    if(s->has_digest && memcmp(s->digest,digest,32))return PXA_STATUS_DENIED;
    s->finished=1;++stream_finishes;return PXA_STATUS_OK;
}
void pxa_host_container_stream_close(pxa_host_container_stream_t *s) {
    if(!s)return;
    if(!s->finished)assert(!rmdir(s->path));
    mbedtls_md_free(&s->hash);free(s->path);free(s);++stream_closes;
}
pxa_status_t pxa_esp_package_store_prepare_install(const uint8_t *prefix,size_t bytes,uint64_t total,uint64_t transfer,uint8_t download_only,
    const uint8_t root[32],const char *app,pxa_esp_package_preview_t *preview,pxa_esp_install_approval_t *placement) {
    assert(bytes==metadata_size && total==body_size && (transfer==0 || transfer==total || (pxa_read_u16(prefix+6)==2 && transfer==metadata_size+pxa_read_u32(prefix+56))));
    assert(!memcmp(prefix,body,bytes) && !strcmp(app,"pxa-store-test"));
    if(bad_metadata || (root && root[0]))return PXA_STATUS_DENIED;
    if(low_space)return PXA_STATUS_QUOTA_EXCEEDED;
    memset(placement,0,sizeof(*placement));placement->count=2;placement->expected_generation=42;placement->download_only=download_only;
    placement->choices[0].volume.mount_epoch=1;placement->choices[1].volume.mount_epoch=2;
    placement->requires_resources=with_resources;
    memset(preview,0,sizeof(*preview));strcpy(preview->app_id,app);preview->has_publisher_root=true;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_prepare_resources(pxa_esp_install_approval_t *placement,
    const uint8_t *prefix,size_t bytes,uint64_t total,uint64_t transfer) {
    assert(with_resources && placement->requires_resources && !http_live && !placement->plan.reserved);
    if(bad_resources)return PXA_STATUS_DENIED;
    if(prefix)assert(bytes==metadata_size && total==resource_size && (transfer==resource_size || (pxa_read_u16(prefix+6)==2 && transfer==metadata_size+pxa_read_u32(prefix+56))) && !memcmp(prefix,resource_body,bytes));
    else {assert(!bytes && !total && !transfer);if(!reuse_resources)return PXA_STATUS_UNAVAILABLE;}
    placement->resource_container_bytes=total;placement->media_count=1;placement->media_selected=0;
    placement->media_choices[0].volume.mount_epoch=3;placement->media_choices[0].reuse=reuse_resources;
    placement->media_fit[1]=1;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_commit_resources(const char *path,pxa_esp_install_approval_t *placement) {
    struct stat info;
    assert(with_resources && !reuse_resources && !http_live && placement->plan.reserved);
    assert(retained[2] && retained[3] && (strstr(path,"/third/PXA/cache/store/.store-") || strstr(path,"/third/PXA/cache/store/.stream-")) && !stat(path,&info));
    assert(S_ISDIR(info.st_mode) || (uint64_t)info.st_size==resource_size);++resource_commits;
    if(!resource_commit_fail && S_ISDIR(info.st_mode))assert(!rmdir(path));
    return resource_commit_fail?PXA_STATUS_DENIED:PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_reserve_install(pxa_esp_install_approval_t *placement,size_t index) {
    assert(index==1);if(reserve_fail)return PXA_STATUS_QUOTA_EXCEEDED;
    placement->plan.reserved=1;placement->plan.app_mount_epoch=2;++reservations;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_commit_install(const char *path,pxa_esp_install_approval_t *placement) {
    assert(approve && waiting_job==NULL && !cancel_prompt && placement->plan.reserved && placement->expected_generation==42 && !http_live);
    assert(strstr(path,"/PXA/cache/store/.store-") || strstr(path,"/PXA/cache/store/.stream-"));++installs;return stale_record?PXA_STATUS_BAD_STATE:PXA_STATUS_OK;
}
static void notified(pxa_esp_store_job_t *job,uint8_t phase) {
    if(phase==1) {assert(!job->approved);++confirmations;waiting_job=job;job->approved=approve;job->placement.selected=1;}
    if(phase==2)++completions;
    if(phase==3) {assert(job->keep_file);waiting_job=job;}
    if(phase==4 && job->downloaded_bytes==0) {
        assert(job->approved && job->placement.plan.reserved && !http_live);
        assert(job->total_bytes==body_size+((with_resources && !reuse_resources)?resource_size:0));
        ++started_progress;
    }
}
#include "../src/package/pxa_esp_store_download.c"
static pxa_esp_store_job_t job(unsigned separate) {
    pxa_esp_store_job_t value={0};value.separate=(uint8_t)separate;value.size=body_size;value.notify=notified;
    strcpy(value.app_id,"pxa-store-test");strcpy(value.ticket,"/api/v2/artifacts/1/download?expires=123&signature=0000000000000000000000000000000000000000000000000000000000000000");
    assert(!mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),body,body_size,value.digest));return value;
}
static bool exists(const pxa_esp_store_job_t *j) {return j->path[0] && !access(j->path,F_OK);}
int main(void) {
    assert(!mkdir(CONFIG_PXA_MOUNT_POINT,0700));body=calloc(1,body_size);assert(body);
    memcpy(body,"PXAC",4);pxa_write_u16(body+6,1);pxa_write_u16(body+8,64);pxa_write_u16(body+14,12);
    pxa_write_u32(body+16,12);pxa_write_u32(body+20,108);pxa_write_u32(body+24,108);
    pxa_write_u64(body+32,body_size-metadata_size);pxa_write_u64(body+48,body_size);pxa_write_u32(body+60,pxa_container_crc32(body,60));
    pxa_esp_store_job_t value;
#if !PXA_ENABLE_LEGACY_CONTAINER
    value=job(0);pxa_esp_store_worker(&value);
    assert(value.status!=PXA_STATUS_OK && !transfers && !confirmations && !exists(&value));
    pxa_write_u16(body+6,2);pxa_write_u32(body+60,pxa_container_crc32(body,60));
#endif
    value=job(0);low_space=true;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_QUOTA_EXCEEDED && !transfers && !confirmations && !exists(&value));low_space=false;
    value=job(0);short_metadata=true;pxa_esp_store_worker(&value);assert(value.status!=PXA_STATUS_OK && !transfers);short_metadata=false;
    value=job(0);bad_metadata=true;pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_DENIED && !transfers);bad_metadata=false;
    value=job(0);value.approved=1;approve=false;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_DENIED && confirmations==1 && !transfers);approve=true;
    value=job(0);cancel_prompt=true;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_CANCELLED && !transfers);cancel_prompt=false;
    value=job(0);reserve_fail=true;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_QUOTA_EXCEEDED && !transfers);reserve_fail=false;
    value=job(0);short_body=true;pxa_esp_store_worker(&value);assert(value.status!=PXA_STATUS_OK && !exists(&value));short_body=false;
    value=job(0);value.digest[0]^=1;pxa_esp_store_worker(&value);assert(value.status!=PXA_STATUS_OK && !exists(&value));
    value=job(0);pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_OK && !exists(&value) && installs==1);
    value=job(0);stale_record=true;pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_BAD_STATE && !exists(&value));stale_record=false;
    value=job(1);pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_OK && exists(&value));
    unsigned before=transfers,started_before=started_progress;value.separate=2;approve=false;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_DENIED && exists(&value) && transfers==before);approve=true;
    stale_record=true;pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_BAD_STATE && exists(&value));stale_record=false;
    pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_OK && !exists(&value) && transfers==before);
    assert(started_progress==started_before); /* Cached install retries have no network transfer. */
    value=job(0);old_server=true;pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_OK && !exists(&value) && metadata_requests>transfers);old_server=false;
    char third[384];snprintf(third,sizeof(third),"%s/third",CONFIG_PXA_MOUNT_POINT);assert(!mkdir(third,0700));
    resource_body=calloc(1,resource_size);assert(resource_body);memcpy(resource_body,body,metadata_size);
    pxa_write_u64(resource_body+32,resource_size-metadata_size);pxa_write_u64(resource_body+48,resource_size);
    pxa_write_u32(resource_body+60,pxa_container_crc32(resource_body,60));with_resources=true;
    before=transfers;unsigned code_installs=installs;
    value=job(0);bad_resources=true;pxa_esp_store_worker(&value);bad_resources=false;
    assert(value.status==PXA_STATUS_DENIED && transfers==before && installs==code_installs);
    value=job(0);resource_commit_fail=true;pxa_esp_store_worker(&value);resource_commit_fail=false;
    assert(value.status==PXA_STATUS_DENIED && transfers==before+2 && installs==code_installs && !exists(&value));
    assert(value.resource_path[0] && access(value.resource_path,F_OK)!=0);
    value=job(0);pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_OK && !exists(&value) && !value.resource_path[0] && installs==code_installs+1);
    assert(value.total_bytes==body_size+resource_size && value.downloaded_bytes==value.total_bytes && resource_commits==2);
    reuse_resources=true;before=transfers;value=job(0);pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_OK && transfers==before+1 && value.total_bytes==body_size && resource_commits==2);
    value=job(1);before=transfers;pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_UNSUPPORTED && !exists(&value) && transfers==before);
    with_resources=false;
    assert(stream_begins==stream_closes);
    stream_begins=stream_closes=stream_finishes=0;
    for(unsigned i=0;i<2;++i) {
        uint8_t *b=i?resource_body:body;size_t n=i?resource_size:body_size;
        pxa_write_u16(b+6,2);pxa_write_u32(b+56,96);pxa_write_u64(b+32,n-metadata_size-96);
        pxa_write_u32(b+60,pxa_container_crc32(b,60));
    }
    value=job(0);pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_OK && !exists(&value) && stream_finishes==1);
    value=job(0);short_body=true;pxa_esp_store_worker(&value);short_body=false;
    assert(value.status!=PXA_STATUS_OK && !exists(&value) && stream_finishes==1);
    value=job(0);value.digest[0]^=1;pxa_esp_store_worker(&value);assert(value.status==PXA_STATUS_DENIED && !exists(&value));
    value=job(0);stream_reject=true;pxa_esp_store_worker(&value);stream_reject=false;
    assert(value.status!=PXA_STATUS_OK && !exists(&value));
    value=job(0);stale_record=true;pxa_esp_store_worker(&value);stale_record=false;
    assert(value.status==PXA_STATUS_BAD_STATE && !exists(&value));
    with_resources=true;reuse_resources=false;value=job(0);pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_OK && !exists(&value) && !value.resource_path[0]);
    with_resources=false;unsigned previous=stream_begins;value=job(1);pxa_esp_store_worker(&value);
    assert(value.status==PXA_STATUS_OK && exists(&value) && stream_begins==previous);assert(!unlink(value.path));
    assert(stream_begins==stream_closes);free(resource_body);
    assert(!retained[2] && !retained[3]);
    assert(reservations==releases && completions==deletes);
    assert(started_progress>0); /* Approval publishes zero-byte progress before network I/O. */
    uint8_t payload[512]={0};value=job(0);size_t a=strlen(value.app_id),t=strlen(value.ticket);payload[0]=(uint8_t)a;
    pxa_write_u16(payload+1,(uint16_t)t);pxa_write_u64(payload+3,body_size);memcpy(payload+43,value.app_id,a);memcpy(payload+43+a,value.ticket,t);
    assert(pxa_esp_store_job_decode(&value,payload,43+a+t)); /* More than old 4 MiB cap. */
    free(body);
    char path[384];snprintf(path,sizeof(path),"%s/PXA/cache/store",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/third/PXA/cache/store",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/third/PXA/cache",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/third/PXA",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));assert(!rmdir(third));
    snprintf(path,sizeof(path),"%s/PXA/cache",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));snprintf(path,sizeof(path),"%s/PXA",CONFIG_PXA_MOUNT_POINT);assert(!rmdir(path));assert(!rmdir(CONFIG_PXA_MOUNT_POINT));
    puts("Store: signed preflight before transfer, native selection, volume lease, >4 MiB, hash/cancel/CAS failures and offline retry passed");
}
