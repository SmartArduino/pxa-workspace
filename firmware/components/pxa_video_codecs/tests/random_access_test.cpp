// SPDX-License-Identifier: Apache-2.0
#include "pxa_h264_limits.hpp"
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
    Decoder(int threads):thread_count(threads) {
        assert(!WelsCreateDecoder(&decoder));
        decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);
        SDecodingParam p{};p.uiTargetDqLayer=255;p.eEcActiveIdc=ERROR_CON_DISABLE;
        p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;assert(!decoder->Initialize(&p));
        int quiet=WELS_LOG_QUIET;decoder->SetOption(DECODER_OPTION_TRACE_LEVEL,&quiet);
    }
    ~Decoder() {decoder->Uninitialize();WelsDestroyDecoder(decoder);}
    void output(uint8_t **p,const SBufferInfo& f) {
        if(!f.iBufferStatus)return;
        uint64_t checksum=0;
        for(int y=0;y<f.UsrData.sSystemBuffer.iHeight;y+=17)
            for(int x=0;x<f.UsrData.sSystemBuffer.iWidth;x+=19)
                checksum=checksum*33+p[0][y*f.UsrData.sSystemBuffer.iStride[0]+x];
        assert(pictures.emplace(f.uiOutYuvTimeStamp,checksum).second);
    }
    void submit(const Unit& bytes,uint64_t pts) {
        uint8_t *p[3]{};SBufferInfo f{};f.uiInBsTimeStamp=pts;
        assert(decoder->DecodeFrameNoDelay(bytes.data(),int(bytes.size()),p,&f)==dsErrorFree);output(p,f);
    }
    void drain() {
        if(thread_count) {uint8_t *p[3]{};SBufferInfo f{};assert(decoder->DecodeFrameNoDelay(nullptr,0,p,&f)==dsErrorFree);output(p,f);}
        for(unsigned i=0;i<32;++i) {uint8_t *p[3]{};SBufferInfo f{};
            assert(decoder->FlushFrame(p,&f)==dsErrorFree);if(!f.iBufferStatus)return;output(p,f);}
        assert(false);
    }
};
int main(int argc,char **argv) {
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);
    Unit bytes{std::istreambuf_iterator<char>(file),{}};assert(!bytes.empty());
    std::vector<Unit> units;size_t begin=0,at=0;
    while(at<bytes.size()) {
        size_t nal=at+(bytes[at+2]==1?3:4),next=pxa_video::start_code(bytes.data(),bytes.size(),nal);
        if((bytes[nal]&31)==9 && at>begin) {units.emplace_back(bytes.begin()+begin,bytes.begin()+at);begin=at;}
        at=next;
    }
    units.emplace_back(bytes.begin()+begin,bytes.end());assert(units.size()==24);
    for(int threads:{0,2}) {
    Decoder baseline(threads),jumped(threads);
    for(size_t i=0;i<units.size();++i)baseline.submit(units[i],1000000+i*41667);
    baseline.drain();assert(baseline.pictures.size()==24);
    for(size_t i=0;i<6;++i)jumped.submit(units[i],i*41667);
    // Jump straight to an IDR, retaining decoder buffers and unrelated AAC state.
    for(size_t i=0;i<units.size();++i)jumped.submit(units[i],1000000+i*41667);
    jumped.drain();
    for(auto [pts,checksum]:baseline.pictures)assert(jumped.pictures.at(pts)==checksum);
    Decoder filtered(threads);unsigned discarded=0;std::map<uint64_t,bool> kept;
    for(size_t i=0;i<units.size();++i) {
        const auto& au=units[i];bool disposable=true,vcl=false;size_t at=0;
        while(at<au.size()) {
            size_t nal=at+(au[at+2]==1?3:4);unsigned type=au[nal]&31;
            if(type==1){vcl=true;if(au[nal]&0x60)disposable=false;}
            else if(type!=6 && type!=9 && type!=10 && type!=11 && type!=12)disposable=false;
            at=pxa_video::start_code(au.data(),au.size(),nal);
        }
        if(vcl && disposable){++discarded;continue;}
        uint64_t pts=1000000+i*41667;kept[pts]=true;filtered.submit(au,pts);
    }
    filtered.drain();assert(discarded && filtered.pictures.size()==kept.size());
    for(auto [pts,_]:kept)assert(filtered.pictures.at(pts)==baseline.pictures.at(pts));
    std::printf("Non-reference frame skipping: %u dropped, all %zu retained pictures unchanged\n",discarded,kept.size());
    std::puts("IDR jump: all 24 reordered pictures match a fresh decoder without reinitialization");
    }
}
