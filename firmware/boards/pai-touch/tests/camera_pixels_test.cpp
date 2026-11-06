// SPDX-License-Identifier: Apache-2.0
#include "pai_touch_camera_pixels.h"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    assert(PaiTouchYuvRgb565(0,128,128)==0);
    assert(PaiTouchYuvRgb565(255,128,128)==0xffff);
    assert(PaiTouchYuvRgb565(76,85,255)==0xf800);
    assert(PaiTouchYuvRgb565(150,44,21)==0x07e0);
    assert(PaiTouchYuvRgb565(29,255,107)==0x001f);
    assert(PaiTouchYuvRgb565(128,128,128)==0x8410);
    const std::array<uint8_t,8> input{0,128,255,128,128,128,64,128};
    std::array<uint16_t,6> output{0xabcd,0,0,0,0,0xfedc};
    assert(PaiTouchYuyvPreview(input,2,2,1,std::span(output).subspan(1,4)));
    assert(output[0]==0xabcd && output[5]==0xfedc);
    assert(output[1]==0 && output[2]==0xffff && output[3]==0x8410 && output[4]==0x4208);
    // Exercise shared chroma, clipping and rounding across all U/V values.
    for (unsigned cb=0; cb<256; ++cb) for (unsigned cr=0; cr<256; ++cr) {
        const uint8_t first=uint8_t(cb+cr),second=uint8_t(cb-cr);
        const std::array<uint8_t,4> pair{first,uint8_t(cb),second,uint8_t(cr)};
        assert(PaiTouchYuyvPreview(pair,2,1,1,output));
        assert(output[0]==PaiTouchYuvRgb565(first,uint8_t(cb),uint8_t(cr)));
        assert(output[1]==PaiTouchYuvRgb565(second,uint8_t(cb),uint8_t(cr)));
    }
    const std::array<uint8_t,16> scaled{0,128,64,128,128,128,255,128,100,128,100,128,100,128,100,128};
    assert(PaiTouchYuyvPreview(scaled,4,2,2,std::span(output).subspan(1,2)));
    assert(output[1]==0 && output[2]==0x8410);
    assert(!PaiTouchYuyvPreview(std::span(input).first(7),2,2,1,output));
    assert(!PaiTouchYuyvPreview(input,2,2,1,std::span(output).first(3)));
    assert(!PaiTouchYuyvPreview(input,3,1,1,output));
    assert(!PaiTouchYuyvPreview(input,2,2,0,output));
    assert(!PaiTouchYuyvPreview(input,2,2,3,output));
    assert(!PaiTouchYuyvPreview(input,UINT32_MAX,UINT32_MAX,1,output));
    puts("PAI camera YUYV: full-range colors, shared chroma, downsampling and bounds passed");
}
