/* SPDX-License-Identifier: MIT */
#include <pxa/app.hpp>
#include <optional>
#include <cstdio>
#include <ctime>
#include "bench_kernel.h"
#ifndef PXA_BENCH_RELEASE_PACING
#define PXA_BENCH_RELEASE_PACING 0
#endif

struct Comparison {
    std::optional<pxa::Surface> surface;
    unsigned cpu=0,sample=0,mode=0,round=0,frames=0,attempts=0;
    std::uint64_t start=0,last=0,paint_us=0,submit_us=0;
    bool ready=false,initializing=false,done=false;
    static std::uint64_t now(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
    void log(pxa::Context& c,const char* text){(void)c.log().write(pxa::LogLevel::info,text);}
    pxa::Task<void> initialize(pxa::Context& c){
        log(c,"BENCH system=pxa test=initializing");
        auto window=co_await c.window().snapshot(true);
        if(!window){log(c,"BENCH system=pxa test=error stage=window");co_return std::unexpected(window.error());}
        auto full=c.window().fullscreen();if(!full)co_return full;
        auto awake=c.window().keep_screen_on(true);if(!awake)co_return awake;
        auto created=co_await c.surface().create_mapped({.width=296,.height=240,.buffers=3,.direct_scanout=true});
        if(!created){char line[100];std::snprintf(line,sizeof(line),"BENCH system=pxa test=error stage=surface status=%d",int(created.error()));log(c,line);co_return std::unexpected(created.error());}
        surface.emplace(std::move(*created));
        auto configured=co_await surface->configure({.width=296,.height=240});
        if(!configured)co_return configured;
        auto frame=surface->acquire();
        if(frame){auto pixels=frame->rgb565();if(pixels){
            bench_paint(reinterpret_cast<uint16_t*>(frame->pixels().data()),296,240,frame->stride_bytes()/2,2,0);
            auto sum=bench_checksum(reinterpret_cast<const uint16_t*>(frame->pixels().data()),296,240,frame->stride_bytes()/2);
            char line[160];std::snprintf(line,sizeof(line),"BENCH system=pxa test=pixels sum=%u",sum);log(c,line);
            (void)frame->present(1);
        }}
        ready=true;log(c,"BENCH system=pxa test=begin version=1 width=296 height=240");co_return pxa::Result<void>{};
    }
    void on_foreground(pxa::Context& c){if(!initializing){initializing=true;(void)c.tasks().start(initialize(c));}}
    pxa::Result<void> on_start(pxa::Context& c){on_foreground(c);return {};}
    void on_error(pxa::Context& c,pxa::Error error){char line[100];std::snprintf(line,sizeof(line),"BENCH system=pxa test=error stage=task status=%d",int(error));log(c,line);}
    pxa::Result<bool> on_event(pxa::Context& c,const pxa::Event& event){
        if constexpr(PXA_BENCH_RELEASE_PACING){
            if(cpu>=5 && event.service==16 && event.opcode==0x8001){on_frame(c,{});return true;}
        }
        return false;
    }
    void on_frame(pxa::Context& c,pxa::game::FrameTick){
        if(!ready||done)return;
        char line[240];
        if(cpu<5){
            auto begin=now();auto sum=bench_kernel(cpu);auto us=now()-begin;
            std::snprintf(line,sizeof(line),"BENCH system=pxa test=cpu kind=%u sample=%u us=%llu sum=%u",cpu,sample,(unsigned long long)us,sum);log(c,line);
            if(++sample==9){sample=0;cpu++;}return;
        }
        auto time=now();
        if(!start){start=last=time;frames=attempts=0;paint_us=submit_us=0;
            std::snprintf(line,sizeof(line),"BENCH system=pxa test=frame_begin mode=%u round=%u",mode,round);log(c,line);}
        if(time-last>=1000000){
            std::snprintf(line,sizeof(line),"BENCH system=pxa test=frames mode=%u round=%u elapsed_us=%llu frames=%u attempts=%u paint_us=%llu submit_us=%llu",mode,round,(unsigned long long)(time-start),frames,attempts,(unsigned long long)paint_us,(unsigned long long)submit_us);log(c,line);last=time;
        }
        if(time-start>=8000000){
            if(++mode==4){mode=0;round++;}start=0;
            if(round==3){done=true;log(c,"BENCH system=pxa test=done");}return;
        }
        attempts++;
        auto frame=surface->acquire();if(!frame)return;
        auto p=frame->rgb565();if(!p)return;
        auto before=now();
        // Presentation-only buffers start with a deterministic constant image.
        bench_paint(reinterpret_cast<uint16_t*>(frame->pixels().data()),296,240,frame->stride_bytes()/2,mode==0?1:mode,mode==0?0:frames);
        auto painted=now();auto submitted=frame->present(2+round*10000+mode*2000+frames);
        auto after=now();paint_us+=painted-before;submit_us+=after-painted;
        if(submitted)frames++;
    }
};
PXA_GAME(Comparison)
