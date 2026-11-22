// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_rgb565.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" void pxa_video_pie_average16(uint8_t*,int32_t,const uint8_t*,int32_t,
    const uint8_t*,int32_t,int32_t,int32_t);
extern "C" void pxa_video_pie_halfpel(const uint8_t*,int32_t,uint8_t*,int32_t,int32_t,int32_t,int32_t);
extern "C" void pxa_video_pie_chroma8(const uint8_t*,int32_t,uint8_t*,int32_t,const uint8_t*,int32_t);
extern "C" bool pxa_video_pack_ppa420(const uint8_t*,const uint8_t*,const uint8_t*,
    unsigned,unsigned,unsigned,unsigned,uint8_t*);
namespace {
__attribute__((naked)) uint32_t read_cfg() {asm volatile("esp.movx.r.cfg a0\nret");}
__attribute__((naked)) uint32_t read_sar() {asm volatile("esp.movx.r.sar a0\nret");}
__attribute__((naked)) void write_cfg(uint32_t) {asm volatile("esp.movx.w.cfg a0\nret");}
__attribute__((naked)) void write_sar(uint32_t) {asm volatile("esp.movx.w.sar a0\nret");}
struct Control {
    uint32_t cfg=read_cfg(),sar=read_sar(),test_cfg=(cfg&~0xf0u)|0x60u;
    Control(){write_cfg(test_cfg);write_sar(13);}
    ~Control(){write_sar(sar);write_cfg(cfg);}
    bool preserved(){return read_cfg()==test_cfg && read_sar()==13;}
};
}

bool pxa_video_pie_device_test() {
    Control control;
    alignas(16) uint8_t a[1024],b[1024],out[1024];
    // All 65536 byte pairs, all misalignments, non-vector-sized row strides,
    // aliasing either source, and canaries around every row.
    for(unsigned offset=0;offset<16;++offset) {
        for(unsigned i=0;i<sizeof(a);++i){a[i]=uint8_t(i*71+offset);b[i]=uint8_t(i*137-offset);}
        std::memset(out,0xa5,sizeof(out));
        pxa_video_pie_average16(out+offset,37,a+offset,35,b+15-offset,39,16,16);
        for(unsigned i=0;i<sizeof(out);++i) {
            unsigned row=i>=offset?(i-offset)/37:16,x=i>=offset?(i-offset)%37:16;
            unsigned expected=row<16 && x<16 ? (unsigned(a[offset+row*35+x])+b[15-offset+row*39+x]+1)/2 : 0xa5;
            if(out[i]!=expected){std::printf("PIE AVG FAIL offset=%u i=%u got=%u expected=%u\n",offset,i,out[i],expected);return false;}
        }
    }
    for(unsigned av=0;av<256;++av)for(unsigned bv=0;bv<256;bv+=16) {
        std::fill_n(a,16,uint8_t(av));for(unsigned i=0;i<16;++i)b[i]=uint8_t(bv+i);
        pxa_video_pie_average16(a,16,a,16,b,16,16,1);
        for(unsigned i=0;i<16;++i)if(a[i]!=(av+bv+i+1)/2)return false;
    }
    for(unsigned i=0;i<256;++i){a[i]=uint8_t(i*57);b[i]=uint8_t(i*31);out[i]=uint8_t((unsigned(a[i])+b[i]+1)/2);}
    pxa_video_pie_average16(b,16,a,16,b,16,16,16);
    if(std::memcmp(out,b,256))return false;
    for(unsigned seed=0;seed<32;++seed)for(unsigned offset=0;offset<16;++offset)
        for(int step:{1,35})for(int width:{8,16}) {
            for(unsigned i=0;i<sizeof(a);++i)a[i]=uint8_t((i*71+(i*i>>3))*seed+i);
            std::memset(out,0xa5,sizeof(out));const auto *src=a+105+offset;
            pxa_video_pie_halfpel(src,35,out+offset,37,width,8,step);
            for(unsigned i=0;i<sizeof(out);++i) {
                unsigned row=i>=offset?(i-offset)/37:8,x=i>=offset?(i-offset)%37:16;
                int expected=0xa5;
                if(row<8 && x<unsigned(width)) {
                    auto *p=src+row*35+x;
                    expected=std::clamp((p[-2*step]+p[3*step]-5*(p[-step]+p[2*step])+20*(p[0]+p[step])+16)>>5,0,255);
                }
                if(out[i]!=expected){std::printf("PIE FILTER FAIL seed=%u off=%u step=%d i=%u got=%u expected=%d\n",
                    seed,offset,step,i,out[i],expected);return false;}
            }
        }

    for(unsigned dx=0;dx<8;++dx)for(unsigned dy=0;dy<8;++dy)for(unsigned offset=0;offset<16;++offset) {
        uint8_t weights[]={uint8_t((8-dx)*(8-dy)),uint8_t(dx*(8-dy)),uint8_t((8-dx)*dy),uint8_t(dx*dy)};
        for(unsigned i=0;i<sizeof(a);++i)a[i]=uint8_t(i*137+offset*57);
        std::memset(out,0xa5,sizeof(out));pxa_video_pie_chroma8(a+offset,35,out+offset,37,weights,8);
        for(unsigned i=0;i<sizeof(out);++i) {
            unsigned row=i>=offset?(i-offset)/37:8,x=i>=offset?(i-offset)%37:8;
            unsigned expected=0xa5;
            if(row<8 && x<8) {auto *p=a+offset+row*35+x;
                expected=(weights[0]*p[0]+weights[1]*p[1]+weights[2]*p[35]+weights[3]*p[36]+32)>>6;}
            if(out[i]!=expected){std::printf("PIE CHROMA FAIL dx=%u dy=%u off=%u i=%u got=%u expected=%u\n",dx,dy,offset,i,out[i],expected);return false;}
        }
    }
    constexpr unsigned w=640,h=360,ys=672,cs=336,bytes=w*h*3/2;
    auto *y=static_cast<uint8_t*>(heap_caps_aligned_alloc(128,ys*h,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    auto *u=static_cast<uint8_t*>(heap_caps_aligned_alloc(128,cs*h/2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    auto *v=static_cast<uint8_t*>(heap_caps_aligned_alloc(128,cs*h/2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    auto *cpu=static_cast<uint8_t*>(heap_caps_aligned_alloc(128,bytes+128,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    auto *pie=static_cast<uint8_t*>(heap_caps_aligned_alloc(128,bytes+128,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    bool ok=y&&u&&v&&cpu&&pie;
    if(ok) {
        for(unsigned i=0;i<ys*h;++i)y[i]=uint8_t(i*57+i/643);
        for(unsigned i=0;i<cs*h/2;++i){u[i]=uint8_t(i*31+i/337);v[i]=uint8_t(i*71+i/113);}
        std::memset(cpu,0xa5,bytes+128);std::memset(pie,0xa5,bytes+128);
        pxa_video::pack_ppa420(y,u,v,w,h,ys,cs,cpu);
        pxa_video_pack_ppa420(y,u,v,w,h,ys,cs,pie);
        ok=std::memcmp(cpu,pie,bytes+128)==0;
        if(!ok)for(unsigned i=0;i<bytes+128;++i)if(cpu[i]!=pie[i]){
            std::printf("PIE PACK FAIL i=%u got=%u expected=%u\n",i,pie[i],cpu[i]);break;
        }
        int64_t times[2]{};
        for(unsigned n=0;n<16;++n)for(unsigned j=0;j<2;++j){unsigned mode=(n+j)&1;
            int64_t start=esp_timer_get_time();
            if(mode)pxa_video_pack_ppa420(y,u,v,w,h,ys,cs,pie);
            else pxa_video::pack_ppa420(y,u,v,w,h,ys,cs,cpu);
            times[mode]+=esp_timer_get_time()-start;
        }
        std::printf("PIE PACK BENCH core=%d cpu_us=%lld pie_us=%lld exact=%d\n",xPortGetCoreID(),
            (long long)(times[0]/16),(long long)(times[1]/16),ok);
    }
    heap_caps_free(y);heap_caps_free(u);heap_caps_free(v);heap_caps_free(cpu);heap_caps_free(pie);
    ok=ok&&control.preserved();
    std::printf("PIE DIFFERENTIAL core=%d %s\n",xPortGetCoreID(),ok?"PASS":"FAIL");
    return ok;
}
