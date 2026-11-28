// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_rgb565.hpp"
#include "pxa_video_box420.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
static uint16_t reference(int y,int u,int v) {
    y=std::max(0,y-16);u-=128;v-=128;
    auto clip=[](int c){return std::max(0,std::min(255,c));};
    return uint16_t((clip((298*y+409*v+128)>>8)>>3)<<11|
        (clip((298*y-100*u-208*v+128)>>8)>>2)<<5|(clip((298*y+516*u+128)>>8)>>3));
}
static void check_box420() {
    // Different strides and padded output exercise alternating UYY and VYY
    // rows, rounding at both stages, and the edge of each active rectangle.
    const uint8_t y[8][5] = {{0,1,16,19}, {3,4,24,25}, {255,255,0,0}, {254,254,1,1},
                              {0,2,4,8}, {1,3,5,9}, {1,2,3,4}, {5,6,7,8}};
    const uint8_t u[4][3] = {{10,20}, {30,40}, {1,2}, {3,4}};
    const uint8_t v[4][4] = {{81,90}, {100,110}, {210,210}, {210,210}};
    const uint8_t *planes[] = {y[0], u[0], v[0]};
    const int32_t strides[] = {5,3,4};
    uint8_t actual[24];
    std::fill_n(actual, sizeof(actual), 0xa5);
    pxa_video::pack_box420(planes, strides, 4, 8, actual, 4);
    const uint8_t expected[] = {
        25,3,22,0xa5,0xa5,0xa5, 96,255,1,0xa5,0xa5,0xa5,
        3,2,7,0xa5,0xa5,0xa5, 210,4,6,0xa5,0xa5,0xa5,
    };
    assert(std::equal(actual, actual + sizeof(actual), expected));
}

int main() {
    check_box420();
    for(int u=0;u<256;++u)for(int v=0;v<256;++v)for(int y=0;y<256;++y)
        assert(pxa_video::rgb565_detail::pixel(uint8_t(y),409*(v-128),-100*(u-128)-208*(v-128),516*(u-128))==reference(y,u,v));
    for(unsigned w:{1,2,3,16,17})for(unsigned h:{1,2,3,8,9}) {
        const unsigned ys=w+7,cs=(w+1)/2+3;
        std::vector<uint8_t> y(ys*h),u(cs*((h+1)/2)),v(u.size()),rgb(w*h*2+16,0xa5);
        for(size_t i=0;i<y.size();++i)y[i]=uint8_t(i*31);
        for(size_t i=0;i<u.size();++i){u[i]=uint8_t(i*43);v[i]=uint8_t(i*67);}
        pxa_video::yuv420_rgb565(y.data(),u.data(),v.data(),w,h,ys,cs,rgb.data());
        for(unsigned j=0;j<h;++j)for(unsigned i=0;i<w;++i) {
            size_t p=(size_t(j)*w+i)*2;
            assert((rgb[p]|unsigned(rgb[p+1])<<8)==reference(y[j*ys+i],u[(j/2)*cs+i/2],v[(j/2)*cs+i/2]));
        }
        for (unsigned dw : {1u,2u,11u}) for (unsigned dh : {1u,3u,12u}) {
            const unsigned stride=dw*2+3;
            std::vector<uint8_t> result(size_t(stride)*dh+1,0xa5);
            pxa_video::scale_yuv420_rgb565(y.data(),u.data(),v.data(),w,h,ys,cs,result.data()+1,dw,dh,stride);
            assert(result[0]==0xa5);
            for (unsigned row=0;row<dh;++row) {
                for (unsigned col=0;col<dw;++col) {
                    const size_t src=((row*h/dh)*w+col*w/dw)*2, dst=1+row*stride+col*2;
                    assert(result[dst]==rgb[src] && result[dst+1]==rgb[src+1]);
                }
                for (unsigned x=dw*2;x<stride;++x) assert(result[1+row*stride+x]==0xa5);
            }
        }
        for(size_t i=w*h*2;i<rgb.size();++i)assert(rgb[i]==0xa5);
    }
    for(unsigned w:{2,4,8,16,18,32,852,854})for(unsigned pad:{0,2,12,30})for(unsigned h:{2,4,8})for(unsigned alignment:{0,1}) {
        const unsigned ys=w+7,cs=w/2+3;const unsigned pitch=w+pad;const size_t bytes=size_t(pitch)*h*3/2;
        std::vector<uint8_t> y(ys*h),u(cs*h/2),v(u.size()),packed(bytes+16,0xa5);
        for(size_t i=0;i<y.size();++i)y[i]=uint8_t(i*31);
        for(size_t i=0;i<u.size();++i){u[i]=uint8_t(i*43);v[i]=uint8_t(i*67);}
        auto *out=packed.data()+alignment;
        assert(pxa_video::pack_ppa420(y.data(),u.data(),v.data(),w,h,ys,cs,out,pitch));
        for(unsigned row=0;row<h;++row)for(unsigned x=0;x<w;x+=2) {
            size_t p=size_t(row)*pitch*3/2+x*3/2;
            assert(out[p]==(row&1?v:u)[size_t(row/2)*cs+x/2]);
            assert(out[p+1]==y[size_t(row)*ys+x] && out[p+2]==y[size_t(row)*ys+x+1]);
        }
        for(unsigned row=0;row<h;++row)for(unsigned i=w*3/2;i<pitch*3/2;++i)
            assert(out[size_t(row)*pitch*3/2+i]==0xa5);
        assert(!pxa_video::pack_ppa420(y.data(),u.data(),v.data(),w,h,ys,cs,out,w-2));
        if(alignment)assert(packed[0]==0xa5);
        for(size_t i=bytes+alignment;i<packed.size();++i)assert(packed[i]==0xa5);
        assert(!pxa_video::pack_ppa420(y.data(),u.data(),v.data(),w-1,h,ys,cs,out,pitch));
    }
    {
        // Half-size Surface output uses aligned word writes, including padded rows.
        constexpr unsigned w=64,h=36,ys=w+3,cs=w/2+5,stride=w+4;
        std::vector<uint8_t> y(ys*h),u(cs*h/2),v(cs*h/2),out(stride*h/2,0xa5);
        for(size_t i=0;i<y.size();++i)y[i]=uint8_t(i*31);
        for(size_t i=0;i<u.size();++i){u[i]=uint8_t(i*43);v[i]=uint8_t(i*67);}
        pxa_video::scale_yuv420_rgb565(y.data(),u.data(),v.data(),w,h,ys,cs,out.data(),w/2,h/2,stride);
        for(unsigned row=0;row<h/2;++row) {
            for(unsigned x=0;x<w/2;++x) {
                const size_t pos=row*stride+2*x;
                assert((out[pos]|unsigned(out[pos+1])<<8)==reference(y[2*row*ys+2*x],u[row*cs+x],v[row*cs+x]));
            }
            for(unsigned i=w;i<stride;++i) assert(out[row*stride+i]==0xa5);
        }
    }
    std::puts("RGB565: all 16777216 YUV triplets and odd/padded images match scalar conversion");
}
