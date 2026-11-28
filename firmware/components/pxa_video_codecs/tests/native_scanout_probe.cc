// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_native_surface.hpp"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_log.h"
#include "esp_cache.h"
#include <algorithm>
#include <cstring>
#include <memory>

namespace {
using pxa_video::native_surface::Picture;
using pxa_video::native_surface::Rect;
constexpr unsigned width = 320, height = 240;
constexpr size_t pixels = width * height, bytes = pixels * 2;
constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
struct Yuv { uint8_t y, u, v; };
constexpr Yuv bars[] = {{235,128,128}, {81,90,240}, {145,54,34}, {41,240,110},
                        {210,16,146}, {106,202,222}, {170,166,16}, {16,128,128}};
unsigned color(unsigned x, unsigned y, unsigned w, unsigned h) {
    return (x*8/w + (y*3/h)*3) % 8;
}
bool close_color(uint16_t p, uint16_t reference) {
    const auto near = [](int a, int b) { return a >= b-1 && a <= b+1; };
    return near(p >> 11, reference >> 11) &&
           near((p >> 5) & 63, (reference >> 5) & 63) &&
           near(p & 31, reference & 31);
}
bool probe(unsigned sw, unsigned sh, unsigned step, uint8_t rotation,
           uint16_t *logical, uint16_t *eager, uint16_t *fused) {
    const unsigned pitch = (sw+31)&~31u;
    const size_t plane_bytes = size_t(pitch)*sh*3/2;
    std::unique_ptr<uint8_t,decltype(&heap_caps_free)> storage(static_cast<uint8_t *>(
        heap_caps_aligned_calloc(128, 1, sizeof(Picture) + plane_bytes*2, caps)), &heap_caps_free);
    if (!storage) return false;
    const unsigned rw = sw*step/16, rh = sh*step/16;
    const Rect region = {(width-rw)/2,(height-rh)/2,rw,rh};
    auto *pic = new(storage.get()) Picture{uint16_t(sw),uint16_t(sh),uint16_t(pitch),
        uint16_t(step),width,height,region};
    auto *packed = reinterpret_cast<uint8_t*>(pic+1);
    auto *y = packed+plane_bytes, *u = y+size_t(pitch)*sh, *v = u+size_t(pitch)*sh/4;
    for (unsigned sy=0; sy<sh; ++sy) for (unsigned sx=0; sx<sw; ++sx) {
        const auto c = bars[color(sx & ~1u,sy & ~1u,sw,sh)];
        y[size_t(sy)*pitch+sx] = c.y;
        if (((sx|sy) & 1u) == 0) {
            u[size_t(sy/2)*(pitch/2)+sx/2] = c.u;
            v[size_t(sy/2)*(pitch/2)+sx/2] = c.v;
        }
    }
    if (!pxa_video_pack_ppa420(y,u,v,sw,sh,pitch,pitch/2,packed,pitch)) return false;
    std::fill_n(logical,pixels,0xf81f);
    std::fill_n(fused,pixels,0x07e0);
    const pxa_esp_surface_native_target_t l{logical,bytes,width*2,width,height,0};
    const pxa_esp_surface_native_target_t n{fused,bytes,height*2,height,width,rotation};
    if (!pxa_video::native_surface::draw(pic,&l) || !pxa_video::native_surface::draw(pic,&n)) return false;
    bool correct_bars = true;
    for (unsigned row=0; row<height; ++row) for (unsigned col=0; col<width; ++col) {
        const size_t idx = rotation == 1 ? col*height + height-1-row : (width-1-col)*height+row;
        const auto pixel = logical[row*width+col];
        eager[idx] = pixel;
        if ((col < region.x || col >= region.x+rw || row < region.y || row >= region.y+rh) && pixel)
            correct_bars = false;
    }
    for (unsigned i=0; i<8; ++i) {
        const auto c=bars[i];
        const auto ix=region.x+(2*i+1)*rw/16, iy=region.y+rh/6;
        const auto expected=pxa_video::rgb565_detail::sample(c.y,c.u,c.v);
        if (!close_color(logical[iy*width+ix],expected)) correct_bars=false;
    }
    size_t different = 0, first = 0;
    for (size_t i=0; i<pixels; ++i) if (eager[i]!=fused[i]) { if (!different) first=i; ++different; }
    const auto crc=[](const uint16_t *p) { return esp_rom_crc32_le(0,reinterpret_cast<const uint8_t*>(p),bytes); };
    ESP_LOGI("S31Scanout","YUV420/BT601 limited %ux%u -> %ux%u+%u+%u, turn=%u RGB565 LE; "
            "eager=%08lx fused=%08lx mismatches=%u first=%u values=%04x/%04x borders_colors=%d",
            sw,sh,rw,rh,region.x,region.y,rotation,(unsigned long)crc(eager),(unsigned long)crc(fused),
            unsigned(different),unsigned(first),eager[first],fused[first],int(correct_bars));
    // A spatial ramp exercises PPA taps on both sides of every strip boundary.
    for(unsigned sy=0;sy<sh;++sy) for(unsigned sx=0;sx<sw;++sx) {
        y[size_t(sy)*pitch+sx]=16+(7*sx+13*sy)%220;
        if(!((sx|sy)&1)) {
            u[size_t(sy/2)*(pitch/2)+sx/2]=16+(sx+17*sy)%224;
            v[size_t(sy/2)*(pitch/2)+sx/2]=16+(11*sx+sy)%224;
        }
    }
    if (!pxa_video_pack_ppa420(y,u,v,sw,sh,pitch,pitch/2,packed,pitch) ||
        !pxa_video::native_surface::draw(pic,&l)) return false;
    std::fill_n(eager,pixels,0);
    if (esp_cache_msync(eager,bytes,ESP_CACHE_MSYNC_FLAG_DIR_C2M)!=ESP_OK) return false;
    for (unsigned sy=0;sy<sh;sy+=32) {
        const unsigned count=std::min(32u,sh-sy);
        ppa_srm_oper_config_t op{};
        op.in.buffer=packed+size_t(sy)*pitch*3/2;
        op.in.pic_w=pitch;op.in.block_w=sw; op.in.pic_h=op.in.block_h=count;
        op.in.srm_cm=PPA_SRM_COLOR_MODE_YUV420;
        op.in.yuv_range=PPA_COLOR_RANGE_LIMIT;op.in.yuv_std=PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        op.out.buffer=eager;op.out.buffer_size=bytes;op.out.pic_w=width;op.out.pic_h=height;
        op.out.block_offset_x=region.x;op.out.block_offset_y=region.y+sy*step/16;
        op.out.srm_cm=PPA_SRM_COLOR_MODE_RGB565;op.scale_x=op.scale_y=step/16.0f;
        op.mode=PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_scale_rotate_mirror(pxa_video::native_surface::scaler(),&op)!=ESP_OK) return false;
    }
    unsigned mismatches=0;
    for(unsigned i=0;i<pixels;++i) mismatches+=logical[i]!=eager[i];
    ESP_LOGI("S31Scanout","stripe full420 %ux%u padded=%u stripe32=%08lx full=%08lx mismatches=%u",
        sw,sh,pitch,(unsigned long)crc(eager),(unsigned long)crc(logical), mismatches);
    return correct_bars && !different && !mismatches;
}
}
static bool verified = false;
extern "C" bool pxa_gamekit_native_verify(uint16_t *logical,uint16_t *eager,uint16_t *fused,uint8_t rotation) {
    const bool small = probe(32,32,16,rotation,logical,eager,fused);
    const bool p360 = probe(640,360,8,rotation,logical,eager,fused);
    const bool p480 = probe(852,480,6,rotation,logical,eager,fused);
    std::fill_n(logical,pixels,0);
    verified=small && p360 && p480;
    ESP_LOGI("S31Scanout","direct %s",verified ? "enabled" : "disabled");
    return verified;
}
extern "C" bool pxa_gamekit_native_verified() { return verified; }
