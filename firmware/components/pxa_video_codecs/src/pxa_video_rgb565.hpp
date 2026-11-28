// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pxa_video {
// P4 PPA's 12-bpp layout: U,Y,Y on even rows and V,Y,Y on odd rows.
inline void pack_ppa420_row(const uint8_t *luma, const uint8_t *chroma,
                            unsigned width, uint8_t *destination) {
    using Word = uint32_t __attribute__((may_alias));
    unsigned x = 0;
    if (!(uintptr_t(destination) & 3)) {
        for (; x + 8 <= width; x += 8) {
            const auto *p = luma + x, *c = chroma + x / 2;
            auto *words = reinterpret_cast<Word*>(destination);
            words[0] = uint32_t(c[0]) | (uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(c[1]) << 24);
            words[1] = uint32_t(p[2]) | (uint32_t(p[3]) << 8) | (uint32_t(c[2]) << 16) | (uint32_t(p[4]) << 24);
            words[2] = uint32_t(p[5]) | (uint32_t(c[3]) << 8) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
            destination += 12;
        }
    }
    for (; x < width; x += 2) {
        *destination++ = chroma[x / 2];
        *destination++ = luma[x];
        *destination++ = luma[x + 1];
    }
}
inline bool pack_ppa420(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                        unsigned width, unsigned height, unsigned ys, unsigned cs,
                        uint8_t *out, unsigned pitch) {
    if (!width || !height || ((width | height | pitch) & 1) || pitch < width ||
        ys < width || cs < width / 2) return false;
    const size_t stride = size_t(pitch) * 3 / 2;
    for (unsigned row = 0; row < height; ++row) {
        pack_ppa420_row(y + size_t(row) * ys, (row & 1 ? v : u) + size_t(row / 2) * cs,
                        width, out + size_t(row) * stride);
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
inline uint16_t sample(uint8_t y, uint8_t u, uint8_t v) {
    const int cb=int(u)-128, cr=int(v)-128;
    return pixel(y, 409*cr, -100*cb-208*cr, 516*cb);
}
inline void store(uint8_t *p,uint16_t c) {p[0]=uint8_t(c);p[1]=uint8_t(c>>8);}
}
// Sample the visible picture directly out of strided 4:2:0 reference planes.
inline void scale_yuv420_rgb565(const uint8_t *y, const uint8_t *u, const uint8_t *v,
    unsigned sw, unsigned sh, unsigned ys, unsigned cs, uint8_t *out,
    unsigned dw, unsigned dh, unsigned stride) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (sw == 2*dw && sh == 2*dh && !(dw & 1) && !((uintptr_t(out) | stride) & 3)) {
        using Word = uint32_t __attribute__((may_alias));
        for (unsigned row=0; row<dh; ++row) {
            const uint8_t *luma=y+size_t(row)*2*ys, *cb=u+size_t(row)*cs, *cr=v+size_t(row)*cs;
            auto *dst=reinterpret_cast<Word*>(out+size_t(row)*stride);
            for (unsigned x=0; x<dw; x+=2) {
                const uint32_t a=rgb565_detail::sample(luma[2*x], cb[x], cr[x]);
                const uint32_t b=rgb565_detail::sample(luma[2*x+2], cb[x+1], cr[x+1]);
                dst[x/2]=a | (b<<16);
            }
        }
        return;
    }
#endif
    const unsigned dx=sw/dw, rx=sw%dw;
    for (unsigned row=0; row<dh; ++row) {
        const unsigned sy=row*sh/dh;
        const uint8_t *luma=y+size_t(sy)*ys;
        const uint8_t *cb_row=u+size_t(sy/2)*cs, *cr_row=v+size_t(sy/2)*cs;
        uint8_t *dst=out+size_t(row)*stride;
        unsigned sx=0, error=0;
        for (unsigned x=0; x<dw; ++x) {
            rgb565_detail::store(dst+x*2, rgb565_detail::sample(luma[sx], cb_row[sx/2], cr_row[sx/2]));
            sx+=dx; error+=rx;
            if (error>=dw) { error-=dw; ++sx; }
        }
    }
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
