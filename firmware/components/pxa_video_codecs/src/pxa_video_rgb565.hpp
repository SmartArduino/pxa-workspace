// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pxa_video {
// P4 PPA's 12-bpp layout: U,Y,Y on even rows and V,Y,Y on odd rows.
inline bool pack_ppa420(const uint8_t *y,const uint8_t *u,const uint8_t *v,
    unsigned width,unsigned height,unsigned ys,unsigned cs,uint8_t *out) {
    if(!width || !height || (width&1) || (height&1))return false;
    using Word=uint32_t __attribute__((may_alias));
    const size_t stride=size_t(width)*3/2;
    for(unsigned row=0;row<height;++row) {
        const auto *luma=y+size_t(row)*ys,*chroma=(row&1?v:u)+size_t(row/2)*cs;
        auto *destination=out+size_t(row)*stride;
        if(!(width&7) && !(uintptr_t(destination)&3)) {
            auto *words=reinterpret_cast<Word*>(destination);
            for(unsigned x=0;x<width;x+=8) {
                const auto *p=luma+x,*c=chroma+x/2;
                *words++=uint32_t(c[0])|(uint32_t(p[0])<<8)|(uint32_t(p[1])<<16)|(uint32_t(c[1])<<24);
                *words++=uint32_t(p[2])|(uint32_t(p[3])<<8)|(uint32_t(c[2])<<16)|(uint32_t(p[4])<<24);
                *words++=uint32_t(p[5])|(uint32_t(c[3])<<8)|(uint32_t(p[6])<<16)|(uint32_t(p[7])<<24);
            }
        } else for(unsigned x=0;x<width;x+=2) {
            *destination++=chroma[x/2];*destination++=luma[x];*destination++=luma[x+1];
        }
    }
    return true;
}
namespace rgb565_detail {
// BT.601 limited-range conversion, with the same rounding as the scalar
// reference. The complete YUV byte domain fits [-258,534] before clipping.
constexpr int bias=320,entries=896;
template<unsigned shift,unsigned discard> constexpr auto channel_table() {
    std::array<uint16_t,entries> table{};
    for(int i=0;i<entries;++i) {
        int c=i-bias;c=c<0?0:c>255?255:c;
        table[i]=uint16_t((unsigned(c)>>discard)<<shift);
    }
    return table;
}
constexpr auto luma_table() {
    std::array<int32_t,256> table{};
    for(int i=0;i<256;++i)table[i]=298*(i>16?i-16:0)+128;
    return table;
}
inline constexpr auto red=channel_table<11,3>(),green=channel_table<5,2>(),blue=channel_table<0,3>();
inline constexpr auto luma=luma_table();
inline uint16_t pixel(uint8_t y,int rc,int gc,int bc) {
    const int yy=luma[y];
    return red[((yy+rc)>>8)+bias]|green[((yy+gc)>>8)+bias]|blue[((yy+bc)>>8)+bias];
}
inline void store(uint8_t *p,uint16_t c) {p[0]=uint8_t(c);p[1]=uint8_t(c>>8);}
}
inline void yuv420_rgb565(const uint8_t *y_plane,const uint8_t *u_plane,const uint8_t *v_plane,
    unsigned width,unsigned height,unsigned y_stride,unsigned chroma_stride,uint8_t *rgb) {
    using namespace rgb565_detail;
    if (!(width&1) && !(reinterpret_cast<uintptr_t>(rgb)&3)) {
        // Two pixels per aligned word store. PSRAM byte stores are costly on
        // the P4; keep the odd-width/unaligned fallback below fully general.
        using Pair=uint32_t __attribute__((may_alias));
        for(unsigned y=0;y<height;y+=2) {
            const uint8_t *a=y_plane+size_t(y)*y_stride,*b=y+1<height?a+y_stride:nullptr;
            const uint8_t *u=u_plane+size_t(y/2)*chroma_stride,*v=v_plane+size_t(y/2)*chroma_stride;
            auto *out=reinterpret_cast<Pair*>(rgb+size_t(y)*width*2);
            auto *below=out+width/2;
            for(unsigned x=0;x<width;x+=2) {
                const int cb=int(u[x/2])-128,cr=int(v[x/2])-128;
                const int rc=409*cr,gc=-100*cb-208*cr,bc=516*cb;
                out[x/2]=uint32_t(pixel(a[x],rc,gc,bc))|(uint32_t(pixel(a[x+1],rc,gc,bc))<<16);
                if(b)below[x/2]=uint32_t(pixel(b[x],rc,gc,bc))|(uint32_t(pixel(b[x+1],rc,gc,bc))<<16);
            }
        }
        return;
    }
    for(unsigned y=0;y<height;y+=2) {
        const uint8_t *a=y_plane+size_t(y)*y_stride;
        const uint8_t *b=y+1<height?a+y_stride:nullptr;
        const uint8_t *u=u_plane+size_t(y/2)*chroma_stride,*v=v_plane+size_t(y/2)*chroma_stride;
        uint8_t *out=rgb+size_t(y)*width*2,*below=out+size_t(width)*2;
        for(unsigned x=0;x<width;x+=2) {
            const int cb=int(u[x/2])-128,cr=int(v[x/2])-128;
            const int rc=409*cr,gc=-100*cb-208*cr,bc=516*cb;
            store(out+x*2,pixel(a[x],rc,gc,bc));
            if(x+1<width)store(out+(x+1)*2,pixel(a[x+1],rc,gc,bc));
            if(b) {
                store(below+x*2,pixel(b[x],rc,gc,bc));
                if(x+1<width)store(below+(x+1)*2,pixel(b[x+1],rc,gc,bc));
            }
        }
    }
}
}
