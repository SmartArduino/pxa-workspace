// SPDX-License-Identifier: Apache-2.0
#include "pxa.h"
#include "pxa_video.h"
#include "pxa_clock.h"
#include "pxa_log.h"
#include "fixtures.h"
#include <stdio.h>
#include <string.h>
static uint64_t handle,total_us;
static uint8_t packet[128*1024],pixels[4096];
static size_t offset;
static unsigned stage,frames,units;
static void log_result(const char *what,int result) {
    char text[160];snprintf(text,sizeof(text),"VIDEO_PROBE %s result=%d frames=%u units=%u decode_us=%llu",
        what,result,frames,units,(unsigned long long)total_us);
    pxa_log_write(result ? PXA_LOG_LEVEL_ERROR : PXA_LOG_LEVEL_INFO,text);
}
static void fail(int status) { log_result("FAIL",status);if(handle)pxa_close_handle(handle);handle=0;stage=99; }
static size_t start(size_t at) {
    for(;at+3<=sizeof(high);++at) if(!high[at] && !high[at+1] &&
        (high[at+2]==1 || (at+4<=sizeof(high) && !high[at+2] && high[at+3]==1)))return at;
    return sizeof(high);
}
static int read_frame(void) {
    pxa_video_frame_t frame;int result=pxa_video_frame_info(handle,&frame);
    if(result==PXA_STATUS_WOULD_BLOCK)return 0;
    if(result)return result;
    unsigned checksum=0;size_t bytes=0;
    while(bytes<frame.frame_bytes) {
        int n=pxa_io(handle,1,pixels,sizeof(pixels));if(n<=0)return n ? n : -100;
        for(int i=0;i<n;++i)checksum=checksum*33+pixels[i];bytes+=(unsigned)n;
    }
    if(!checksum || bytes!=frame.frame_bytes)return -101;
    if(stage==1 && (frame.width!=640 || frame.height!=360))return -102;
    if(stage==3 && (frame.width!=320 || frame.height!=180))return -103;
    ++frames;total_us+=frame.decode_us;return 0;
}
static void pump(void) {
    if(stage==7) {stage=0;pxa_video_request_open(10,0,640,360);}
    else if(stage==1) {
        size_t next=offset;
        if(offset<sizeof(high)) {
            size_t at=offset;
            while(at<sizeof(high)) {
                size_t nal=at+(high[at+2]==1?3:4);next=start(nal);
                if(next<sizeof(high)) {size_t p=next+(high[next+2]==1?3:4);if((high[p]&31)==9)break;}
                at=next;
            }
            if(next-offset+16>sizeof(packet)) {fail(-104);return;}
            memcpy(packet+16,high+offset,next-offset);
            int result=pxa_video_decode(handle,packet,(unsigned)(next-offset+16),units*41667,0);
            if(result){fail(result);return;}offset=next;++units;
            result=read_frame();if(result)fail(result);
        } else {
            unsigned before=frames;int result=pxa_video_decode(handle,packet,16,0,1);
            if(!result)result=read_frame();if(result){fail(result);return;}
            if(frames==before) {
                if(frames!=24 || units!=24){fail(-105);return;}
                log_result("H264_HIGH_PASS",0);pxa_close_handle(handle);handle=0;
                stage=2;pxa_video_request_open(20,1,640,360);
            }
        }
    } else if(stage==3) {
        memcpy(packet+16,jpeg,sizeof(jpeg));int result=pxa_video_decode(handle,packet,sizeof(jpeg)+16,0,0);
        if(!result)result=read_frame();if(result){fail(result);return;}
        log_result("MJPEG_PASS",0);
        for(size_t at=0;at<sizeof(aac);) {
            if(sizeof(aac)-at<7){fail(-109);return;}
            size_t n=((aac[at+3]&3u)<<11)|((unsigned)aac[at+4]<<3)|(aac[at+5]>>5);
            if(n<7 || n>sizeof(aac)-at){fail(-110);return;}
            memcpy(packet+8,aac+at,n);
            result=pxa_video_audio_decode(handle,packet,n+8,at);
            pxa_video_audio_info_t audio_info;
            if(!result)result=pxa_video_audio_info(handle,&audio_info);
            if(result || audio_info.sample_rate!=44100 || audio_info.channels!=2 || !audio_info.frame_bytes){fail(-111);return;}
            unsigned nonzero=0;size_t read=0;
            while(read<audio_info.frame_bytes) {
                int bytes=pxa_io(handle,PXA_VIDEO_IO_AUDIO_READ,pixels,sizeof(pixels));
                if(bytes<=0){fail(-112);return;}
                for(int i=0;i<bytes;++i)nonzero+=pixels[i]!=0;
                read+=(unsigned)bytes;
            }
            if(!nonzero){fail(-113);return;}at+=n;
        }
        log_result("AAC_PASS",0);pxa_close_handle(handle);handle=0;
        stage=4;pxa_video_request_open(30,0,320,180);
    } else if(stage==5) {
        size_t end=start(4); /* SPS may follow AUD: submitting the whole bounded fixture also exercises the early limit. */
        (void)end;memcpy(packet+16,high,sizeof(high));
        int result=pxa_video_decode(handle,packet,sizeof(high)+16,0,0);
        if(result!=PXA_STATUS_LIMIT_EXCEEDED){fail(-106);return;}
        pxa_close_handle(handle);handle=0;stage=6;log_result("LIMIT_PASS_ALL_DONE",0);
    }
}
int32_t pxa_app_start(const uint8_t *config,uint32_t size) {
    (void)config;(void)size;pxa_clock_set_period(PXA_CLOCK_MIN_PERIOD_MS);pxa_video_request_capability(1,0);return 0;
}
int32_t pxa_app_on_event(const uint8_t *data,uint32_t size) {
    pxa_event_t e;if(!pxa_parse_event(data,size,&e))return PXA_EVENT_UNHANDLED;
    pxa_video_capability_t cap;
    if(pxa_video_parse_capability(&e,1,&cap)) {
        if(cap.status || cap.codec!=1 || cap.profiles!=7 || cap.recommended_height!=360){fail(-107);return PXA_EVENT_HANDLED;}
        stage=7;
    }
    if(e.service==24 && e.opcode==2) {
        if(e.payload_size<4){fail(-108);return PXA_EVENT_HANDLED;}
        if((int32_t)pxa_load_u32(e.payload)==PXA_STATUS_DENIED){stage=7;return PXA_EVENT_HANDLED;}
        if(pxa_load_u32(e.payload)){fail((int32_t)pxa_load_u32(e.payload));return PXA_EVENT_HANDLED;}
        if(e.payload_size!=12){fail(-114);return PXA_EVENT_HANDLED;}
        handle=pxa_load_u64(e.payload+4);stage=e.token==10?1:e.token==20?3:5;
    }
    uint64_t tick;if(pxa_clock_parse_tick(&e,&tick))pump();return PXA_EVENT_HANDLED;
}
void pxa_app_stop(uint32_t reason) {(void)reason;handle=0;}
