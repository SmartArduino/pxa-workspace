// SPDX-License-Identifier: Apache-2.0
#include "pxa_h264_limits.hpp"
#include "wels/codec_api.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc,char **argv) {
    assert(argc==2 || argc==3);
    unsigned expected=argc==3 ? unsigned(std::strtoul(argv[2],nullptr,10)) : 24;
    assert(expected>0);
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}}; assert(bytes.size()>100);
    assert(pxa_video::validate_annexb(bytes.data(),bytes.size(),640,360)==0);
    assert(pxa_video::validate_annexb(bytes.data(),bytes.size(),320,180)==PXA_STATUS_LIMIT_EXCEEDED);
    assert(pxa_video::validate_annexb(bytes.data()+1,10,640,360)==PXA_STATUS_PROTOCOL_ERROR);
    const uint8_t bad[]={0,0,1,0xe7};
    assert(pxa_video::validate_annexb(bad,sizeof(bad),640,360)==PXA_STATUS_PROTOCOL_ERROR);
    ISVCDecoder *decoder=nullptr; assert(!WelsCreateDecoder(&decoder));
#ifndef PXA_VIDEO_TEST_THREADS
#define PXA_VIDEO_TEST_THREADS 0
#endif
    int threads=PXA_VIDEO_TEST_THREADS,quiet=WELS_LOG_QUIET;
    decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);
    decoder->SetOption(DECODER_OPTION_TRACE_LEVEL,&quiet);
    SDecodingParam params{};params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;
    params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; assert(!decoder->Initialize(&params));
    unsigned frames=0,units=0; uint64_t checksum=0;
    auto output=[&](uint8_t **planes,const SBufferInfo& info) {
        if (!info.iBufferStatus) return;
        assert(info.UsrData.sSystemBuffer.iWidth==640 && info.UsrData.sSystemBuffer.iHeight==360);
        assert(planes[0] && planes[1] && planes[2]); ++frames;
        for (unsigned y=0;y<360;y+=17) for (unsigned x=0;x<640;x+=19)
            checksum=checksum*33+planes[0][y*info.UsrData.sSystemBuffer.iStride[0]+x];
    };
    size_t begin=0,at=0;
    while (at<bytes.size()) {
        const size_t nal=at+(bytes[at+2]==1 ? 3 : 4);
        const size_t next=pxa_video::start_code(bytes.data(),bytes.size(),nal);
        if ((bytes[nal]&31)==9 && at>begin) {
            uint8_t *planes[3]{};SBufferInfo info{}; info.uiInBsTimeStamp=units*41667;
            assert(decoder->DecodeFrameNoDelay(bytes.data()+begin,int(at-begin),planes,&info)==dsErrorFree);
            ++units;output(planes,info);begin=at;
        } at=next;
    }
    uint8_t *planes[3]{};SBufferInfo info{};info.uiInBsTimeStamp=units*41667;
    assert(decoder->DecodeFrameNoDelay(bytes.data()+begin,int(bytes.size()-begin),planes,&info)==dsErrorFree);
    ++units;output(planes,info);
    if(threads>0){uint8_t *p[3]{};SBufferInfo f{};assert(decoder->DecodeFrameNoDelay(nullptr,0,p,&f)==dsErrorFree);output(p,f);}
    for (unsigned i=0;i<32;++i) { uint8_t *p[3]{}; SBufferInfo f{};
        assert(decoder->FlushFrame(p,&f)==dsErrorFree);if (!f.iBufferStatus) break;output(p,f); }
    assert(units==expected && frames==expected && checksum);
    decoder->Uninitialize();WelsDestroyDecoder(decoder);
    std::printf("High Profile with B frames: %u access units, %u decoded frames, checksum=%llu\n",units,frames,(unsigned long long)checksum);
}
