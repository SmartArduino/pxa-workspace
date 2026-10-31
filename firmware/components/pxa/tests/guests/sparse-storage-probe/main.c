#include "pxa.h"
#include "pxa_fs.h"
#include <string.h>

static uint64_t token;
static unsigned stage, file_index;
static int restored, finished;
static uint8_t buffer[4096];
static const char *names[]={"Save.sav","save.sav","next.sav"};

static int fail(const char *why) {
    finished=1;(void)pxa_log_write(PXA_LOG_LEVEL_ERROR,why);return PXA_STATUS_INTERNAL;
}
static int open_file(int reading) {
    const char *name=names[file_index];
    return pxa_fs_request_open(++token,name,strlen(name),reading?PXA_FS_OPEN_READ:
        PXA_FS_OPEN_WRITE|PXA_FS_OPEN_CREATE|PXA_FS_OPEN_TRUNCATE);
}
int32_t pxa_app_start(const uint8_t *config,uint32_t length) {
    (void)config;(void)length;token=0;stage=file_index=0;restored=finished=0;
    return pxa_fs_request_path(PXA_FS_STAT,++token,names[0],8);
}
int32_t pxa_app_on_event(const uint8_t *bytes,uint32_t length) {
    pxa_event_t event;
    if(!pxa_parse_event(bytes,length,&event))return PXA_STATUS_PROTOCOL_ERROR;
    if(finished || event.service!=PXA_FS_SERVICE || event.token!=token)return PXA_EVENT_UNHANDLED;
    if(stage==0) {
        pxa_fs_stat_result_t result;
        if(!pxa_fs_parse_stat(&event,token,&result))return fail("SPARSE FAIL stat event");
        if(!result.status) {
            if(result.kind!=PXA_FS_KIND_REGULAR || result.size!=262144)return fail("SPARSE FAIL saved size");
            restored=1;stage=3;
        } else if(result.status==PXA_STATUS_NOT_FOUND)stage=1;
        else return fail("SPARSE FAIL stat status");
        if(open_file(restored))return fail("SPARSE FAIL open request");
    } else if(stage==2) {
        int32_t status;
        if(!pxa_fs_parse_status(&event,token,PXA_FS_REPLACE,&status) || status)
            return fail("SPARSE FAIL replace");
        file_index=0;stage=3;
        if(open_file(1))return fail("SPARSE FAIL verify open");
    } else {
        pxa_fs_open_result_t result;
        if(!pxa_fs_parse_open(&event,token,&result) || result.status)return fail("SPARSE FAIL open");
        const unsigned pattern=stage==1?file_index+1:file_index==0?3:2;
        for(unsigned block=0;block<64;++block) {
            if(stage==1) {
                for(unsigned i=0;i<sizeof(buffer);++i)buffer[i]=(uint8_t)(pattern*17+block+i);
                if(pxa_fs_write(result.handle,buffer,sizeof(buffer))!=sizeof(buffer))return fail("SPARSE FAIL write");
            } else {
                if(pxa_fs_read(result.handle,buffer,sizeof(buffer))!=sizeof(buffer))return fail("SPARSE FAIL read");
                for(unsigned i=0;i<sizeof(buffer);++i)
                    if(buffer[i]!=(uint8_t)(pattern*17+block+i))return fail("SPARSE FAIL bytes");
            }
        }
        if(stage==3 && pxa_fs_read(result.handle,buffer,1)!=0)return fail("SPARSE FAIL eof");
        if(pxa_fs_close(result.handle))return fail("SPARSE FAIL close");
        if(++file_index<(stage==1?3u:2u)) {
            if(open_file(stage==3))return fail("SPARSE FAIL next open");
        } else if(stage==1) {
            uint32_t size;stage=2;
            if(!pxa_fs_build_replace(buffer,sizeof(buffer),++token,names[2],8,names[0],8,&size) ||
                pxa_control_packet(buffer,size))return fail("SPARSE FAIL replace request");
        } else {
            finished=1;
            (void)pxa_log_write(PXA_LOG_LEVEL_INFO,restored?
                "SPARSE PASS restored=1 bytes=524288":"SPARSE PASS created=1 bytes=524288");
        }
    }
    return PXA_EVENT_HANDLED;
}
void pxa_app_stop(uint32_t reason) {(void)reason;}
