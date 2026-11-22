// SPDX-License-Identifier: Apache-2.0
#include "pxa_h264_limits.hpp"
#include "pxa_video_dpb.hpp"
#include "wels/codec_api.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
#include <cstdio>
using Unit=std::vector<uint8_t>;
struct Decoder {
    ISVCDecoder *decoder=nullptr;
    std::map<uint64_t,uint64_t> pictures;
    int thread_count;
    Decoder(int threads=0):thread_count(threads) {
        assert(!WelsCreateDecoder(&decoder));
        decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);
        SDecodingParam p{};p.uiTargetDqLayer=255;p.eEcActiveIdc=ERROR_CON_DISABLE;
        p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;assert(!decoder->Initialize(&p));
        int quiet=WELS_LOG_QUIET;decoder->SetOption(DECODER_OPTION_TRACE_LEVEL,&quiet);
    }
    ~Decoder() {decoder->Uninitialize();WelsDestroyDecoder(decoder);}
    void output(uint8_t **p,const SBufferInfo& f) {
        if(!f.iBufferStatus)return;
        uint64_t checksum=14695981039346656037ull;
        for(unsigned plane=0;plane<3;++plane) {
            const int width=plane ? (f.UsrData.sSystemBuffer.iWidth+1)/2 : f.UsrData.sSystemBuffer.iWidth;
            const int height=plane ? (f.UsrData.sSystemBuffer.iHeight+1)/2 : f.UsrData.sSystemBuffer.iHeight;
            for(int y=0;y<height;++y)for(int x=0;x<width;++x)
                checksum=(checksum^p[plane][y*f.UsrData.sSystemBuffer.iStride[plane?1:0]+x])*1099511628211ull;
        }
        assert(pictures.emplace(f.uiOutYuvTimeStamp,checksum).second);
    }
    void submit(const Unit& bytes,uint64_t pts) {
        uint8_t *p[3]{};SBufferInfo f{};f.uiInBsTimeStamp=pts;
        assert(decoder->DecodeFrameNoDelay(bytes.data(),int(bytes.size()),p,&f)==dsErrorFree);output(p,f);
    }
    void drain() {
        if(thread_count){uint8_t *p[3]{};SBufferInfo f{};assert(decoder->DecodeFrameNoDelay(nullptr,0,p,&f)==dsErrorFree);output(p,f);}
        for(unsigned i=0;i<32;++i) {uint8_t *p[3]{};SBufferInfo f{};
            assert(decoder->FlushFrame(p,&f)==dsErrorFree);if(!f.iBufferStatus)return;output(p,f);}
        assert(false);
    }
};
int main(int argc,char **argv) {
    using pxa_video::threaded_picture_count;
    assert(threaded_picture_count(6,4,6,true,2)==14);
    assert(threaded_picture_count(6,4,6,false,2)==18);
    assert(threaded_picture_count(6,4,3,true,2)==18);
    assert(threaded_picture_count(6,7,6,true,2)==18);
    assert(threaded_picture_count(6,4,100,true,2)==18);
    assert(threaded_picture_count(16,16,16,true,2)==18);
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);
    Unit bytes{std::istreambuf_iterator<char>(file),{}};assert(!bytes.empty());
    std::vector<Unit> units;size_t begin=0,at=0;
    while(at<bytes.size()) {
        size_t nal=at+(bytes[at+2]==1?3:4),next=pxa_video::start_code(bytes.data(),bytes.size(),nal);
        if((bytes[nal]&31)==9 && at>begin) {units.emplace_back(bytes.begin()+begin,bytes.begin()+at);begin=at;}
        at=next;
    }
    units.emplace_back(bytes.begin()+begin,bytes.end());assert(!units.empty());
    Decoder baseline;
    for(size_t i=0;i<units.size();++i)baseline.submit(units[i],i*41667);
    baseline.drain();
    for(unsigned repetition=0;repetition<3;++repetition) {
    Decoder threaded(2);
    size_t first_vcl=0;
    while(first_vcl<units[0].size()) {
        size_t nal=first_vcl+(units[0][first_vcl+2]==1?3:4);
        unsigned type=units[0][nal]&31;
        if(type>=1 && type<=5)break;
        first_vcl=pxa_video::start_code(units[0].data(),units[0].size(),nal);
    }
    assert(first_vcl>0);if(repetition&1)threaded.submit(Unit(units[0].begin(),units[0].begin()+first_vcl),0);
    for(size_t i=0;i<units.size();++i)threaded.submit(units[i],i*41667);
    threaded.drain();
    assert(baseline.pictures.size()==units.size() && threaded.pictures.size()==units.size());
    // Check all visible Y/U/V bytes, keyed by the returned PTS. The upstream
    // threaded path has previously returned complete but corrupted pictures.
    assert(baseline.pictures==threaded.pictures);
    }
    std::printf("Two workers: %zu pictures match single-thread Y/U/V output in all three runs (including separate headers and EOF)\n",units.size());
}
