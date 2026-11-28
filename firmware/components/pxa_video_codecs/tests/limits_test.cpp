// SPDX-License-Identifier: Apache-2.0
#include "pxa_h264_limits.hpp"
#include <cassert>
#include <vector>
struct Writer {
    std::vector<uint8_t> bytes{0x67};
    unsigned bits=0;
    void put(unsigned value,unsigned count=1) {
        while(count--){if(!(bits%8))bytes.push_back(0);bytes.back()|=((value>>count)&1)<<(7-bits%8);++bits;}
    }
    void ue(unsigned value){unsigned n=0;for(unsigned v=value+1;v>>=1;)++n;put(0,n);put(value+1,n+1);}
};
std::vector<uint8_t> sps(unsigned level,unsigned width=640,unsigned height=360,unsigned depth=0,unsigned chroma=1,unsigned refs=4,bool progressive=true) {
    Writer w;w.put(100,8);w.put(0,8);w.put(level,8);w.ue(0);
    w.ue(chroma);w.ue(depth);w.ue(depth);w.put(0);w.put(0);
    w.ue(0);w.ue(0);w.ue(0);w.ue(refs);w.put(0);
    w.ue((width+15)/16-1);w.ue((height+15)/16-1);w.put(progressive);w.put(1);
    w.put(1);w.ue(0);w.ue(0);w.ue(0);w.ue(((height+15)/16*16-height)/2);
    w.put(0);w.put(1);while(w.bits%8)w.put(0);
    return w.bytes;
}
int main(){
    const uint8_t packet[]{0,0,1,0x65,0,0,3,1,0,0,0,0,1,0x06,1,0};
    for (size_t end=0; end<=sizeof(packet); ++end) {
        for (size_t from=0; from<=end; ++from) {
            size_t expected=from;
            while (expected+3<=end) {
                const auto *s=packet+expected;
                if (!s[0] && !s[1] && (s[2]==1 || (!s[2] && expected+4<=end && s[3]==1))) break;
                ++expected;
            }
            if (expected+3>end) expected=end;
            assert(pxa_video::start_code(packet,end,from)==expected);
        }
    }
    auto check=[](auto data){return pxa_video::check_sps(data.data(),data.size(),640,360);};
    assert(check(sps(40))==PXA_STATUS_OK);
    assert(check(sps(51))==PXA_STATUS_OK);
    assert(check(sps(51,1280,720))==PXA_STATUS_LIMIT_EXCEEDED);
    assert(check(sps(51,640,360,2))==PXA_STATUS_UNSUPPORTED);
    assert(check(sps(51,640,360,0,2))==PXA_STATUS_UNSUPPORTED);
    assert(check(sps(51,640,360,0,1,17))==PXA_STATUS_UNSUPPORTED);
    assert(check(sps(51,640,360,0,1,4,false))==PXA_STATUS_UNSUPPORTED);
    assert(check(sps(52))==PXA_STATUS_UNSUPPORTED);
    assert(check(sps(49))==PXA_STATUS_UNSUPPORTED);
}
