/* SPDX-License-Identifier: MIT */
#include <aster/aster.h>
#include "bench_kernel.h"
static aster_handle_t surface;
static aster_surface_buffer_t pixels;
static unsigned cpu,sample,mode,round_no,frames;
static uint32_t started,last,paint_ms,submit_ms;
static void log_cpu(unsigned kind,unsigned sample,uint32_t us,uint32_t sum)
{
    /* Keep the guest CRT small; formatting is provided by the Aster SDK import. */
    extern void bench_report_cpu(unsigned,unsigned,uint32_t,uint32_t);
    bench_report_cpu(kind,sample,us,sum);
}
ASTER_APP_EXPORT int aster_app_init(void)
{
    aster_surface_desc_t desc={sizeof(desc),1,296,240,ASTER_PIXEL_RGB565,0};
    int r=aster_surface_create(&desc,sizeof(desc),&surface);if(r)return r;
    pixels=(aster_surface_buffer_t){.size=sizeof(pixels),.version=1};
    r=aster_surface_acquire(surface,&pixels,sizeof(pixels));if(r)return r;
    extern void bench_report_pixels(uint32_t);
    bench_paint(pixels.ptr,296,240,pixels.pitch/2,2,0);
    bench_report_pixels(bench_checksum(pixels.ptr,296,240,pixels.pitch/2));
    aster_app_set_framerate(60);return 0;
}
ASTER_APP_EXPORT void aster_app_event(const aster_event_t *event)
{
    if(event->type!=ASTER_EVENT_FRAME)return;
    uint32_t time=aster_time_now_ms();
    if(cpu<5){uint32_t sum=bench_kernel(cpu);log_cpu(cpu,sample,(aster_time_now_ms()-time)*1000,sum);
        if(++sample==9){sample=0;cpu++;}return;}
    extern void bench_report_frames(unsigned,unsigned,uint32_t,unsigned,uint32_t,uint32_t);
    if(!started){started=last=time;frames=paint_ms=submit_ms=0;extern void bench_report_begin(unsigned,unsigned);bench_report_begin(mode,round_no);}
    if(time-last>=1000){bench_report_frames(mode,round_no,(time-started)*1000,frames,paint_ms*1000,submit_ms*1000);last=time;}
    if(time-started>=8000){
        if(++mode==4){mode=0;round_no++;}started=0;
        if(round_no==3){static const char done[]="BENCH system=aster test=done\n";aster_log(2,done,sizeof(done)-1);aster_app_exit(0);}return;
    }
    uint32_t before=aster_time_now_ms();
    bench_paint(pixels.ptr,296,240,pixels.pitch/2,mode==0?1:mode,mode==0?0:frames);
    uint32_t painted=aster_time_now_ms();int r=aster_surface_present(surface);
    paint_ms+=painted-before;submit_ms+=aster_time_now_ms()-painted;if(!r)frames++;
}
ASTER_APP_EXPORT void aster_app_destroy(void){aster_surface_destroy(surface);}
ASTER_APP_EXPORT void aster_app_start(void){}
ASTER_APP_EXPORT void aster_app_stop(void){}
ASTER_APP_EXPORT void aster_app_pause(void){}
ASTER_APP_EXPORT void aster_app_resume(void){}
