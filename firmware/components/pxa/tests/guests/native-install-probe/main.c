#include "pxa.h"
#include "pxa_fs_roots.h"
static unsigned stage;
static uint64_t media;
static int restored;
static uint8_t value[]="native install data";
static int failed(void) {
    (void)pxa_log_write(PXA_LOG_LEVEL_ERROR,"NATIVE INSTALL FAIL media/private");
    return PXA_STATUS_INTERNAL;
}
int32_t pxa_app_start(const uint8_t *config,uint32_t length) {
    (void)config;(void)length;stage=0;media=0;restored=0;
    return pxa_fs_request_app_media(1);
}
int32_t pxa_app_on_event(const uint8_t *data,uint32_t length) {
    pxa_event_t event;
    if(!pxa_parse_event(data,length,&event))return PXA_STATUS_PROTOCOL_ERROR;
    if(event.service!=PXA_FS_SERVICE || event.token!=stage+1u)return PXA_EVENT_UNHANDLED;
    if(stage==0) {
        pxa_fs_root_result_t root;
        if(!pxa_fs_parse_root(&event,1,&root) || root.status)return failed();
        media=root.handle;stage=1;
        if(pxa_fs_request_open_at(2,media,"assets/install.txt",18,PXA_FS_OPEN_READ))return failed();
    } else if(stage==1) {
        pxa_fs_at_result_t file;uint8_t bytes[32];static const char expected[]="native files resource";
        if(!pxa_fs_parse_open_at(&event,2,&file) || file.status)return failed();
        int32_t count=pxa_fs_read(file.handle,bytes,sizeof(bytes));
        int good=count==(int32_t)sizeof(expected)-1;
        for(unsigned i=0;good && i<sizeof(expected)-1u;++i)if(bytes[i]!=(uint8_t)expected[i])good=0;
        if(pxa_fs_close(file.handle) || pxa_fs_close(media) || !good)return failed();
        media=0;stage=2;
        if(pxa_fs_request_path(PXA_FS_STAT,3,"saved",5))return failed();
    } else if(stage==2) {
        pxa_fs_stat_result_t result;
        if(!pxa_fs_parse_stat(&event,3,&result))return failed();
        if(!result.status) {
            if(result.kind!=PXA_FS_KIND_REGULAR || result.size!=sizeof(value)-1u)return failed();
            restored=1;
        } else if(result.status!=PXA_STATUS_NOT_FOUND)return failed();
        stage=3;
        if(pxa_fs_request_open(4,"saved",5,restored?PXA_FS_OPEN_READ:
            PXA_FS_OPEN_WRITE|PXA_FS_OPEN_CREATE|PXA_FS_OPEN_TRUNCATE))return failed();
    } else if(stage==3) {
        pxa_fs_open_result_t file;uint8_t bytes[sizeof(value)];
        if(!pxa_fs_parse_open(&event,4,&file) || file.status)return failed();
        int good;
        if(restored) {
            good=pxa_fs_read(file.handle,bytes,sizeof(bytes))==(int32_t)sizeof(value)-1;
            for(unsigned i=0;good && i<sizeof(value)-1u;++i)if(bytes[i]!=value[i])good=0;
        } else good=pxa_fs_write(file.handle,value,sizeof(value)-1u)==(int32_t)sizeof(value)-1;
        if(pxa_fs_close(file.handle) || !good)return failed();
        stage=4;(void)pxa_log_write(PXA_LOG_LEVEL_INFO,restored?
            "NATIVE INSTALL PASS media/private restored=1":"NATIVE INSTALL PASS media/private created=1");
    }
    return PXA_EVENT_HANDLED;
}
void pxa_app_stop(uint32_t reason) {(void)reason;}
