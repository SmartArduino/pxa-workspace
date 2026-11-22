// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_memory.h"
#include "wels/codec_api.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <unordered_set>
#include <mutex>
#include "pxa_video_threads.h"
#include <cerrno>
#include <fstream>
#include <iterator>
#include <vector>
static std::unordered_set<void*> live;
static size_t calls,failure;
static std::mutex allocation_mutex;
static int thread_calls,thread_failure=-1;
void pxa_video_worker_error() {}
int pxa_video_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void*),void *arg) {
    if(thread_calls++==thread_failure)return EAGAIN;
    return pthread_create(t,a,fn,arg);
}
int pxa_video_pthread_join(pthread_t t,void **r) {return pthread_join(t,r);}
void *pxa_video_malloc(size_t bytes) {
    std::lock_guard<std::mutex> guard(allocation_mutex);
    if(calls++==failure) return nullptr;
    void *p=std::malloc(bytes); if(p)live.insert(p);return p;
}
void pxa_video_free(void *p) {
    std::lock_guard<std::mutex> guard(allocation_mutex);
    if(p) {assert(live.erase(p)==1);std::free(p);}
}
int main(int argc,char **argv) {
    for(size_t stop=0;stop<24;++stop) {
        assert(live.empty());calls=0;failure=stop;
        ISVCDecoder *d=nullptr;long result=WelsCreateDecoder(&d);
        if(!result) {
            assert(d);int32_t threads=2;d->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);SDecodingParam params{};params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;
            params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
            result=d->Initialize(&params);
            assert(stop<calls ? result!=0 : result==0);
            d->Uninitialize();WelsDestroyDecoder(d);
        } else assert(stop==0 && !d);
        assert(live.empty());
    }
    for(int fail=0;fail<2;++fail) {
        failure=size_t(-1);calls=0;thread_calls=0;thread_failure=fail;
        ISVCDecoder *d=nullptr;assert(!WelsCreateDecoder(&d));int32_t threads=2;
        d->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);SDecodingParam params{};
        params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
        assert(d->Initialize(&params)!=0);d->Uninitialize();WelsDestroyDecoder(d);assert(live.empty());
    }
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    size_t end=4;
    for(;end+5<bytes.size();++end)if(!bytes[end] && !bytes[end+1] &&
        ((bytes[end+2]==1 && (bytes[end+3]&31)==9) ||
         (!bytes[end+2] && bytes[end+3]==1 && (bytes[end+4]&31)==9)))break;
    assert(end<bytes.size());failure=size_t(-1);thread_failure=-1;
    for(unsigned attempt=0;attempt<3;++attempt) {
        ISVCDecoder *d=nullptr;assert(!WelsCreateDecoder(&d));int threads=2;
        d->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);SDecodingParam params{};
        params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
        assert(!d->Initialize(&params));uint8_t *planes[3]{};SBufferInfo info{};
        assert(d->DecodeFrameNoDelay(bytes.data(),int(end),planes,&info)==dsErrorFree);
        // Destruction must join active workers before releasing their pictures.
        WelsDestroyDecoder(d);assert(live.empty());
    }
    for(size_t stop=0;stop<192;++stop) {
        std::fprintf(stderr,"Picture allocation failure %zu\n",stop);
        failure=size_t(-1);ISVCDecoder *d=nullptr;assert(!WelsCreateDecoder(&d));int threads=2;
        d->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);SDecodingParam params{};
        params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
        assert(!d->Initialize(&params));failure=calls+stop;uint8_t *planes[3]{};SBufferInfo info{};
        (void)d->DecodeFrameNoDelay(bytes.data(),int(end),planes,&info);
        WelsDestroyDecoder(d);assert(live.empty());
    }
    std::puts("Control allocations: injected failures and early destruction release every allocation");
}
