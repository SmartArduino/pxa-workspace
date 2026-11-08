// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_camera_geometry.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdio>
int main() {
    constexpr unsigned sw=1920, sh=1080, stride=1924;
    std::vector<uint16_t> source(stride*sh,0xdead);
    for(unsigned y=0;y<sh;++y) for(unsigned x=0;x<sw;++x) source[y*stride+x]=uint16_t(x+31*y);
    for(auto [w,h]:std::array<std::pair<unsigned,unsigned>,8>{{{160,90},{320,180},{480,270},{640,360},{800,450},{960,540},{1280,720},{1920,1080}}}) {
        std::vector<uint16_t> columns(w), output(w*h+2,0xbeef);
        espp4_a::CameraColumns(columns.data(),sw,w);
        espp4_a::CameraResize(source.data(),sw,sh,stride,output.data()+1,w,h,columns.data());
        assert(output.front()==0xbeef && output.back()==0xbeef);
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
            assert(output[1+y*w+x]==source[(uint64_t(y)*sh/h)*stride+uint64_t(x)*sw/w]);
    }
    // Widescreen previews need no column map or intermediate image.
    std::vector<uint16_t> preview(320*180), fast(320*240), columns(320);
    espp4_a::CameraResize(source.data(),sw,sh,stride,preview.data(),320,180,nullptr);
    assert(preview.back()==source[1074*stride+1914]);
    // The 4:3 native-pixel mode crops centrally instead of stretching faces.
    espp4_a::CameraColumns(columns.data(),1440,320);
    espp4_a::CameraResize(source.data()+240,1440,sh,stride,fast.data(),320,240,columns.data());
    for(unsigned y=0;y<240;++y) for(unsigned x=0;x<320;++x)
        assert(fast[y*320+x]==source[(uint64_t(y)*1080/240)*stride+240+uint64_t(x)*1440/320]);
    const uint16_t tiny[]{1,2,3,4};
    std::array<uint16_t,3> up_columns;
    std::array<uint16_t,15> up;
    espp4_a::CameraColumns(up_columns.data(),2,3);
    espp4_a::CameraResize(tiny,2,2,2,up.data(),3,5,up_columns.data());
    for(unsigned y=0;y<5;++y) for(unsigned x=0;x<3;++x) assert(up[y*3+x]==tiny[(y*2/5)*2+x*2/3]);
    puts("camera profiles: integral/fractional/native resize, padded rows and direct preview passed");
}
