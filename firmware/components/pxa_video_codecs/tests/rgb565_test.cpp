// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_rgb565.hpp"
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
int main() {
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
        for(size_t i=w*h*2;i<rgb.size();++i)assert(rgb[i]==0xa5);
    }
    for(unsigned w:{2,4,8,16,18})for(unsigned h:{2,4,8})for(unsigned alignment:{0,1}) {
        const unsigned ys=w+7,cs=w/2+3;const size_t bytes=size_t(w)*h*3/2;
        std::vector<uint8_t> y(ys*h),u(cs*h/2),v(u.size()),packed(bytes+16,0xa5);
        for(size_t i=0;i<y.size();++i)y[i]=uint8_t(i*31);
        for(size_t i=0;i<u.size();++i){u[i]=uint8_t(i*43);v[i]=uint8_t(i*67);}
        auto *out=packed.data()+alignment;
        assert(pxa_video::pack_ppa420(y.data(),u.data(),v.data(),w,h,ys,cs,out));
        for(unsigned row=0;row<h;++row)for(unsigned x=0;x<w;x+=2) {
            size_t p=size_t(row)*w*3/2+x*3/2;
            assert(out[p]==(row&1?v:u)[size_t(row/2)*cs+x/2]);
            assert(out[p+1]==y[size_t(row)*ys+x] && out[p+2]==y[size_t(row)*ys+x+1]);
        }
        if(alignment)assert(packed[0]==0xa5);
        for(size_t i=bytes+alignment;i<packed.size();++i)assert(packed[i]==0xa5);
        assert(!pxa_video::pack_ppa420(y.data(),u.data(),v.data(),w-1,h,ys,cs,out));
    }
    std::puts("RGB565: all 16777216 YUV triplets and odd/padded images match scalar conversion");
}
