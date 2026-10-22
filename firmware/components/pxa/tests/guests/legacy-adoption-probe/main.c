#include "pxa.h"
#include "pxa_fs_roots.h"
#include "pxa_permission.h"
#include "pxa_storage.h"

static uint64_t media;
static int restored;
static uint8_t packet[128];
static uint8_t save[]="legacy adoption private data";
static const uint8_t progress[]="level-7";
static const char key[]="legacy-progress";

static int equal(const uint8_t *a,const uint8_t *b,size_t length) {
    for(size_t i=0;i<length;++i)if(a[i]!=b[i])return 0;
    return 1;
}
static int failed(const char *message) {
    (void)pxa_log_write(PXA_LOG_LEVEL_ERROR,message);return PXA_STATUS_INTERNAL;
}
static int sent(int32_t status) {
    return status?failed("LEGACY PROBE FAIL request"):PXA_EVENT_HANDLED;
}
static int permission(uint16_t opcode,uint64_t token,int network) {
    const char *name=network?"network.http":"audio.playback";
    uint32_t size=0;
    if(!pxa_permission_build(packet,sizeof(packet),opcode,token,name,network?12:14,
        network?NULL:(const uint8_t *)"media",network?0:5,&size))return PXA_STATUS_INTERNAL;
    return pxa_control_packet(packet,size);
}
int32_t pxa_app_start(const uint8_t *config,uint32_t length) {
    (void)config;(void)length;media=0;restored=0;
    return pxa_fs_request_app_media(1);
}
int32_t pxa_app_on_event(const uint8_t *data,uint32_t length) {
    pxa_event_t event;
    if(!pxa_parse_event(data,length,&event))return PXA_STATUS_PROTOCOL_ERROR;
    if(event.token==1 && event.service==PXA_FS_SERVICE) {
        pxa_fs_root_result_t root;
        if(!pxa_fs_parse_root(&event,1,&root) || root.status)return failed("LEGACY PROBE FAIL media root");
        media=root.handle;
        return sent(pxa_fs_request_open_at(2,media,"assets/install.txt",18,PXA_FS_OPEN_READ));
    }
    if(event.token==2 && event.service==PXA_FS_SERVICE) {
        pxa_fs_at_result_t file;uint8_t bytes[32];static const uint8_t expected[]="native files resource";
        if(!pxa_fs_parse_open_at(&event,2,&file) || file.status)return failed("LEGACY PROBE FAIL media open");
        int good=pxa_fs_read(file.handle,bytes,sizeof(bytes))==(int32_t)sizeof(expected)-1;
        if(pxa_fs_close(file.handle) || pxa_fs_close(media) || !good || !equal(bytes,expected,sizeof(expected)-1))
            return failed("LEGACY PROBE FAIL media content");
        media=0;return sent(pxa_fs_request_path(PXA_FS_STAT,3,"saved",5));
    }
    if(event.token==3 && event.service==PXA_FS_SERVICE) {
        pxa_fs_stat_result_t result;
        if(!pxa_fs_parse_stat(&event,3,&result))return failed("LEGACY PROBE FAIL private stat");
        if(!result.status) {
            if(result.kind!=PXA_FS_KIND_REGULAR || result.size!=sizeof(save)-1)return failed("LEGACY PROBE FAIL private size");
            restored=1;
        } else if(result.status!=PXA_STATUS_NOT_FOUND)return failed("LEGACY PROBE FAIL private unavailable");
        return sent(pxa_fs_request_open(4,"saved",5,restored?PXA_FS_OPEN_READ:
            PXA_FS_OPEN_WRITE|PXA_FS_OPEN_CREATE|PXA_FS_OPEN_TRUNCATE));
    }
    if(event.token==4 && event.service==PXA_FS_SERVICE) {
        pxa_fs_open_result_t file;uint8_t bytes[sizeof(save)];int good;
        if(!pxa_fs_parse_open(&event,4,&file) || file.status)return failed("LEGACY PROBE FAIL private open");
        if(restored)good=pxa_fs_read(file.handle,bytes,sizeof(bytes))==(int32_t)sizeof(save)-1 && equal(bytes,save,sizeof(save)-1);
        else good=pxa_fs_write(file.handle,save,sizeof(save)-1)==(int32_t)sizeof(save)-1;
        if(pxa_fs_close(file.handle) || !good)return failed("LEGACY PROBE FAIL private content");
        return sent(pxa_storage_request_get(5,key,sizeof(key)-1));
    }
    if(event.token==5 && event.service==PXA_STORAGE_SERVICE) {
        pxa_storage_get_result_t result;
        if(!pxa_storage_parse_get(&event,5,&result))return failed("LEGACY PROBE FAIL KV response");
        if(result.status==PXA_STATUS_NOT_FOUND && !restored)
            return sent(pxa_storage_request_set(packet,sizeof(packet),6,key,sizeof(key)-1,progress,sizeof(progress)-1));
        if(result.status || !restored || result.value.size!=sizeof(progress)-1 || !equal(result.value.data,progress,sizeof(progress)-1))
            return failed("LEGACY PROBE FAIL KV content");
        return sent(permission(PXA_PERMISSION_CHECK,7,0));
    }
    if(event.token==6 && event.service==PXA_STORAGE_SERVICE) {
        int32_t status;
        if(!pxa_storage_parse_status(&event,6,PXA_STORAGE_SET,&status) || status)return failed("LEGACY PROBE FAIL KV persist");
        return sent(permission(PXA_PERMISSION_CHECK,7,0));
    }
    if(event.token==7 && event.service==PXA_PERMISSION_SERVICE) {
        pxa_permission_check_result_t result;
        if(!pxa_permission_parse_check(&event,7,&result) || result.status)return failed("LEGACY PROBE FAIL permission check");
        if(result.decision==1)return sent(permission(PXA_PERMISSION_CHECK,9,1));
        /* Never repair a lost grant on a restored installation: that would
         * hide a conversion bug behind another user approval. */
        if(restored)return failed("LEGACY PROBE FAIL restored permission lost");
        (void)pxa_log_write(PXA_LOG_LEVEL_INFO,"LEGACY PROBE waiting for initial audio permission");
        return sent(permission(PXA_PERMISSION_ACQUIRE,8,0));
    }
    if(event.token==8 && event.service==PXA_PERMISSION_SERVICE) {
        pxa_permission_acquire_result_t result;
        if(!pxa_permission_parse_acquire(&event,8,&result) || result.status)return failed("LEGACY PROBE FAIL initial permission");
        return sent(permission(PXA_PERMISSION_CHECK,9,1));
    }
    if(event.token==9 && event.service==PXA_PERMISSION_SERVICE) {
        pxa_permission_check_result_t result;
        if(!pxa_permission_parse_check(&event,9,&result) || result.status || result.decision)
            return failed("LEGACY PROBE FAIL undelegated network permission");
        (void)pxa_log_write(PXA_LOG_LEVEL_INFO,restored?
            "LEGACY PROBE PASS media/private/KV audio=allow network=deny restored=1":
            "LEGACY PROBE PASS media/private/KV audio=allow network=deny created=1");
        return PXA_EVENT_HANDLED;
    }
    return PXA_EVENT_UNHANDLED;
}
void pxa_app_stop(uint32_t reason) {(void)reason;}
