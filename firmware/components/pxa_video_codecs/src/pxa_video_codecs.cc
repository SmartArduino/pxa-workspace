// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_codecs.h"
#include "sdkconfig.h"
#include "pxa_video_memory.h"
#include "pxa_video_threads.h"
#include "pxa_h264_limits.hpp"
extern "C" void pxa_video_convert_rgb565(const uint8_t *,const uint8_t *,const uint8_t *,
    unsigned,unsigned,unsigned,unsigned,uint8_t *);
extern "C" bool pxa_video_pack_ppa420(const uint8_t *,const uint8_t *,const uint8_t *,
    unsigned,unsigned,unsigned,unsigned,uint8_t *);
#include "pxa_esp_resource_memory.h"
#include "wels/codec_api.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#endif
#include "pxa_codec_memory.h"
#include "esp_aac_dec.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
thread_local const pxa_memory_allocator_t *allocation_owner,*internal_owner;
thread_local std::atomic<int> *allocation_fault;
struct Scope {
    const pxa_memory_allocator_t *previous,*previous_internal;
    std::atomic<int> *previous_fault;
    explicit Scope(const pxa_memory_allocator_t *owner,const pxa_memory_allocator_t *internal=nullptr,std::atomic<int> *fault=nullptr)
        : previous(allocation_owner),previous_internal(internal_owner),previous_fault(allocation_fault) {allocation_owner=owner;internal_owner=internal;allocation_fault=fault;}
    ~Scope() {allocation_owner=previous;internal_owner=previous_internal;allocation_fault=previous_fault;}
};
const pxa_video_capability_t caps[] = {
    {PXA_VIDEO_H264, PXA_VIDEO_SOFTWARE, 7, 640, 360, 1280, 1280, 1280*720, 512*1024, 5, pxa_video::h264_max_level, 1, 1},
    {PXA_VIDEO_MJPEG, PXA_VIDEO_SOFTWARE, 0, 640, 360, 1920, 1920, 1920*1080, 1024*1024, 25, 0, 2, 1},
};
struct Decoder {
    const pxa_memory_allocator_t *allocator = nullptr;
    const pxa_memory_allocator_t *audio_allocator = nullptr,*internal_allocator=nullptr;
    ISVCDecoder *h264 = nullptr;
    jpeg_dec_handle_t jpeg = nullptr;
    uint8_t *rgb = nullptr;
    uint8_t *rgb_storage = nullptr,*yuv_storage = nullptr,*packed_yuv = nullptr;
    size_t yuv_capacity = 0;
#if CONFIG_SOC_PPA_SUPPORTED
    ppa_client_handle_t ppa = nullptr;
#endif
    bool ppa_disabled = false;
    uint8_t *parameters = nullptr;
    size_t parameters_size = 0;
    int max_workers = PXA_VIDEO_H264_WORKERS, workers = 0;
    std::atomic<int> fault{0};
    bool seen_vcl = false;
    size_t rgb_capacity = 0;
    uint16_t width = 0, height = 0, index = 0;
    void *aac = nullptr;
    uint8_t *pcm = nullptr;
    esp_audio_dec_info_t audio_info{};
    uint32_t pcm_size = 0, pcm_offset = 0;
    uint64_t audio_pts = 0, codec_us = 0, conversion_us = 0;
    unsigned frames = 0;bool draining=false;
};
void destroy(void *value) {
    auto *d = static_cast<Decoder*>(value);
    if (!d) return;
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
    if (d->h264) { WelsDestroyDecoder(d->h264); }
    if (d->jpeg) jpeg_dec_close(d->jpeg);
#if CONFIG_SOC_PPA_SUPPORTED
    if(d->ppa)ppa_unregister_client(d->ppa);
#endif
    PxaCodecMemoryScope audio_scope(d->audio_allocator);
    if (d->aac) esp_aac_dec_close(d->aac);
    pxa_memory_release(d->pcm);
    pxa_memory_release(d->rgb_storage); pxa_memory_release(d->yuv_storage);
    pxa_memory_release(d->parameters); delete d;
}
pxa_status_t initialize_h264(Decoder *d) {
    for(int count:{d->max_workers,0}) {
        if(WelsCreateDecoder(&d->h264) || !d->h264)continue;
        int32_t threads=count;d->h264->SetOption(DECODER_OPTION_NUM_OF_THREADS,&threads);
        SDecodingParam params{};params.uiTargetDqLayer=255;params.eEcActiveIdc=ERROR_CON_DISABLE;
        params.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC;
        if(!d->h264->Initialize(&params)) {
            int trace=WELS_LOG_QUIET;d->h264->SetOption(DECODER_OPTION_TRACE_LEVEL,&trace);
            d->workers=count;
            ESP_LOGI("PxaVideo","H264 decoder workers=%d",count);return PXA_STATUS_OK;
        }
        WelsDestroyDecoder(d->h264);d->h264=nullptr;
    }
    return PXA_STATUS_RESOURCE_LIMIT;
}
pxa_status_t open_decoder(void *, uint16_t index, uint16_t width, uint16_t height, void **out) {
    *out = nullptr;
    if (index >= 2) return PXA_STATUS_UNSUPPORTED;
    auto *d = new(std::nothrow) Decoder;
    if (!d) return PXA_STATUS_RESOURCE_LIMIT;
    d->width=width; d->height=height; d->index=index;
    if(size_t(width)*height>640*360)d->max_workers=0;
    d->allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_IMAGE);
    if (!d->allocator) { delete d; return PXA_STATUS_RESOURCE_LIMIT; }
    d->internal_allocator=pxa_esp_resource_allocator(PXA_MEMORY_INTERNAL,PXA_MEMORY_IMAGE);
    d->audio_allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_AUDIO);
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
    pxa_status_t status=PXA_STATUS_OK;
    if (!index) status=initialize_h264(d);
    else {
        jpeg_dec_config_t config=DEFAULT_JPEG_DEC_CONFIG();
        config.output_type=JPEG_PIXEL_FORMAT_RGB565_LE;
        if (jpeg_dec_open(&config,&d->jpeg) != JPEG_ERR_OK) status=PXA_STATUS_RESOURCE_LIMIT;
    }
    if (status) { destroy(d); return status; }
    *out=d; return PXA_STATUS_OK;
}
bool convert_on_ppa(Decoder *d,uint8_t **planes,unsigned width,unsigned height,unsigned ys,unsigned cs) {
#if CONFIG_SOC_PPA_SUPPORTED
    if(d->ppa_disabled || (width&1) || (height&1))return false;
    if(!d->ppa) {
        ppa_client_config_t config{};config.oper_type=PPA_OPERATION_SRM;
        config.max_pending_trans_num=1;config.data_burst_length=PPA_DATA_BURST_LENGTH_128;
        if(ppa_register_client(&config,&d->ppa)!=ESP_OK){d->ppa_disabled=true;return false;}
    }
    size_t bytes=size_t(width)*height*3/2;
    if(bytes>d->yuv_capacity) {
        pxa_memory_release(d->yuv_storage);d->yuv_storage=nullptr;d->packed_yuv=nullptr;d->yuv_capacity=0;
        d->yuv_storage=static_cast<uint8_t*>(pxa_memory_allocate(d->allocator,bytes+255));
        if(!d->yuv_storage)return false;
        d->packed_yuv=reinterpret_cast<uint8_t*>((uintptr_t(d->yuv_storage)+127)&~uintptr_t(127));
        d->yuv_capacity=bytes;
    }
    if(!pxa_video_pack_ppa420(planes[0],planes[1],planes[2],width,height,ys,cs,d->packed_yuv))return false;
    ppa_srm_oper_config_t operation{};
    operation.in.buffer=d->packed_yuv;operation.in.pic_w=operation.in.block_w=width;
    operation.in.pic_h=operation.in.block_h=height;operation.in.srm_cm=PPA_SRM_COLOR_MODE_YUV420;
    operation.in.yuv_range=PPA_COLOR_RANGE_LIMIT;operation.in.yuv_std=PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    operation.out.buffer=d->rgb;operation.out.buffer_size=(size_t(width)*height*2+127)&~size_t(127);
    operation.out.pic_w=width;operation.out.pic_h=height;operation.out.srm_cm=PPA_SRM_COLOR_MODE_RGB565;
    operation.scale_x=operation.scale_y=1;operation.mode=PPA_TRANS_MODE_BLOCKING;
    if(ppa_do_scale_rotate_mirror(d->ppa,&operation)==ESP_OK)return true;
    d->ppa_disabled=true;ESP_LOGW("PxaVideo","PPA color conversion unavailable; using CPU conversion");
#endif
    return false;
}
pxa_status_t decode(void *value, const uint8_t *data, size_t size, uint64_t pts,
    uint32_t flags, pxa_video_frame_t *frame) {
    auto *d=static_cast<Decoder*>(value); Scope scope(d->allocator,d->internal_allocator,&d->fault);
    if(int fault=d->fault.exchange(0))return pxa_status_t(fault);
    const int64_t started=esp_timer_get_time();
    uint16_t width=0,height=0;
    uint8_t *planes[3]{}; SBufferInfo info{};
    jpeg_dec_io_t jpeg{}; jpeg_dec_header_info_t header{};
    if (!d->index) {
        DECODING_STATE state;
        if (flags) {
            state=dsErrorFree;
            if(!d->draining) {d->draining=true;state=d->h264->DecodeFrameNoDelay(nullptr,0,planes,&info);}
            if(!(int(state)&~int(dsFramePending)) && !info.iBufferStatus)state=d->h264->FlushFrame(planes,&info);
        } else {
            d->draining=false;
            auto status=pxa_video::validate_annexb(data,size,d->width,d->height); if (status) return status;
            bool vcl=false;
            for(size_t at=0;at<size;) {
                size_t nal=at+(data[at+2]==1?3:4);unsigned type=data[nal]&31;
                vcl|=type>=1 && type<=5;
                at=pxa_video::start_code(data,size,nal);
            }
            if(!d->seen_vcl && !vcl && d->workers) {
                if(size>512*1024-d->parameters_size)return PXA_STATUS_LIMIT_EXCEEDED;
                auto *saved=static_cast<uint8_t*>(pxa_memory_allocate(d->allocator,d->parameters_size+size));
                if(!saved)return PXA_STATUS_RESOURCE_LIMIT;
                if(d->parameters_size)std::memcpy(saved,d->parameters,d->parameters_size);
                std::memcpy(saved+d->parameters_size,data,size);
                pxa_memory_release(d->parameters);d->parameters=saved;d->parameters_size+=size;
            }
            info.uiInBsTimeStamp=pts;
            state=d->h264->DecodeFrameNoDelay(data,int(size),planes,&info);
            if(d->fault.load()==PXA_STATUS_RESOURCE_LIMIT)state=dsOutOfMemory;
            if(vcl && !d->seen_vcl && d->workers && (int(state)&~int(dsFramePending))) {
                // Picture allocations happen after SPS parsing, not at open.
                // Preserve streams that fit only the single-worker budget.
                ESP_LOGW("PxaVideo","First-picture state=0x%x; retrying single-thread decoding",unsigned(state));
                WelsDestroyDecoder(d->h264);d->h264=nullptr;d->max_workers=0;d->fault.store(0);
                auto retry=initialize_h264(d);if(retry)return retry;
                if(d->parameters_size) {
                    SBufferInfo header{};
                    state=d->h264->DecodeFrameNoDelay(d->parameters,int(d->parameters_size),planes,&header);
                    if(int(state)&~int(dsFramePending))return PXA_STATUS_PROTOCOL_ERROR;
                }
                info={};info.uiInBsTimeStamp=pts;
                state=d->h264->DecodeFrameNoDelay(data,int(size),planes,&info);
            }
            if(vcl) {
                d->seen_vcl=true;pxa_memory_release(d->parameters);d->parameters=nullptr;d->parameters_size=0;
            }
        }
        if (int(state) & ~int(dsFramePending)) {
            ESP_LOGW("PxaVideo", "H264 decode state=0x%x bytes=%u pts_ms=%llu", unsigned(state), unsigned(size),
                (unsigned long long)(pts/1000));
            return (state & dsOutOfMemory) ? PXA_STATUS_RESOURCE_LIMIT : PXA_STATUS_PROTOCOL_ERROR;
        }
        if (!info.iBufferStatus) return PXA_STATUS_OK;
        width=info.UsrData.sSystemBuffer.iWidth; height=info.UsrData.sSystemBuffer.iHeight;
        pts=info.uiOutYuvTimeStamp;
    } else {
        if (flags) return PXA_STATUS_OK;
        jpeg.inbuf=const_cast<uint8_t*>(data); jpeg.inbuf_len=int(size);
        if (jpeg_dec_parse_header(d->jpeg,&jpeg,&header) != JPEG_ERR_OK) return PXA_STATUS_PROTOCOL_ERROR;
        width=header.width; height=header.height;
    }
    if (!width || !height || width>d->width || height>d->height) return PXA_STATUS_LIMIT_EXCEEDED;
    const int64_t conversion_started=esp_timer_get_time();
    d->codec_us+=conversion_started-started;
    size_t bytes=size_t(width)*height*2;
    if (bytes>d->rgb_capacity) {
        // Retire the previous image before allocating the next; old frame views
        // are invalidated by every decode, including a failed decode.
        pxa_memory_release(d->rgb_storage); d->rgb_storage=d->rgb=nullptr; d->rgb_capacity=0;
        d->rgb_storage=static_cast<uint8_t*>(pxa_memory_allocate(d->allocator,bytes+255));
        if (!d->rgb_storage) return PXA_STATUS_RESOURCE_LIMIT;
        d->rgb=reinterpret_cast<uint8_t*>((uintptr_t(d->rgb_storage)+127)&~uintptr_t(127));
        d->rgb_capacity=bytes;
    }
    if (!d->index) {
        if (!planes[0] || !planes[1] || !planes[2]) return PXA_STATUS_PROTOCOL_ERROR;
        const int ys=info.UsrData.sSystemBuffer.iStride[0], cs=info.UsrData.sSystemBuffer.iStride[1];
        if (ys<int(width) || cs<int((width+1)/2)) return PXA_STATUS_PROTOCOL_ERROR;
        if(!convert_on_ppa(d,planes,width,height,ys,cs))
            pxa_video_convert_rgb565(planes[0],planes[1],planes[2],width,height,ys,cs,d->rgb);
    } else {
        int required=0;
        if (jpeg_dec_get_outbuf_len(d->jpeg,&required) != JPEG_ERR_OK || required<=0 || size_t(required)>bytes)
            return PXA_STATUS_UNSUPPORTED;
        jpeg.outbuf=d->rgb;
        if (jpeg_dec_process(d->jpeg,&jpeg) != JPEG_ERR_OK) return PXA_STATUS_PROTOCOL_ERROR;
    }
    const int64_t ended=esp_timer_get_time();
    d->conversion_us+=ended-conversion_started;
    if (++d->frames%32==0) ESP_LOGI("PxaVideo", "frames=%u codec_ms=%llu conversion_ms=%llu", d->frames,
        (unsigned long long)(d->codec_us/1000),(unsigned long long)(d->conversion_us/1000));
    *frame={width,height,uint32_t(width)*2,pts,uint64_t(ended-started),d->rgb,bytes};
    return PXA_STATUS_OK;
}
pxa_status_t reset(void *value) {
    auto *d=static_cast<Decoder*>(value); Scope scope(d->allocator,d->internal_allocator,&d->fault);
    PxaCodecMemoryScope audio_scope(d->audio_allocator);
    d->pcm_size=d->pcm_offset=0;d->draining=false;
    d->seen_vcl=false;pxa_memory_release(d->parameters);d->parameters=nullptr;d->parameters_size=0;
    if (d->aac) { esp_aac_dec_close(d->aac); d->aac=nullptr; }
    if (d->h264) { WelsDestroyDecoder(d->h264); d->h264=nullptr; d->fault.store(0); return initialize_h264(d); }
    return PXA_STATUS_OK;
}
int32_t audio_io(void *value,uint32_t op,uint8_t *data,size_t size) {
    auto *d=static_cast<Decoder*>(value);
    PxaCodecMemoryScope scope(d->audio_allocator);
    if (op==PXA_VIDEO_IO_AUDIO_DECODE) {
        d->pcm_size=d->pcm_offset=0;
        if (size<15 || size>8192 || data[8]!=0xff || (data[9]&0xf6)!=0xf0) return PXA_STATUS_INVALID_ARGUMENT;
        const size_t adts_size=((data[11]&3u)<<11)|(unsigned(data[12])<<3)|(data[13]>>5);
        if (adts_size!=size-8 || (data[14]&3)) return PXA_STATUS_PROTOCOL_ERROR;
        if (!d->aac && esp_aac_dec_open(nullptr,0,&d->aac)!=ESP_AUDIO_ERR_OK) return PXA_STATUS_RESOURCE_LIMIT;
        if (!d->pcm) d->pcm=static_cast<uint8_t*>(pxa_memory_allocate(d->audio_allocator,16384));
        if (!d->pcm) return PXA_STATUS_RESOURCE_LIMIT;
        esp_audio_dec_in_raw_t raw{}; raw.buffer=data+8; raw.len=size-8;
        esp_audio_dec_out_frame_t out{}; out.buffer=d->pcm; out.len=16384;
        auto err=esp_aac_dec_decode(d->aac,&raw,&out,&d->audio_info);
        if (err!=ESP_AUDIO_ERR_OK || raw.consumed!=size-8 || !out.decoded_size || out.decoded_size>16384)
            return err==ESP_AUDIO_ERR_MEM_LACK ? PXA_STATUS_RESOURCE_LIMIT : PXA_STATUS_PROTOCOL_ERROR;
        if (d->audio_info.bits_per_sample!=16 || !d->audio_info.channel || d->audio_info.channel>2 ||
            !d->audio_info.sample_rate || d->audio_info.sample_rate>96000 ||
            out.decoded_size%(d->audio_info.channel*2)) return PXA_STATUS_UNSUPPORTED;
        d->pcm_size=out.decoded_size; d->audio_pts=pxa_read_u64(data); return 0;
    }
    if (!d->pcm_size) return PXA_STATUS_WOULD_BLOCK;
    if (op==PXA_VIDEO_IO_AUDIO_INFO) {
        if (size!=24) return PXA_STATUS_INVALID_ARGUMENT;
        std::memset(data,0,size); pxa_write_u32(data,d->audio_info.sample_rate);
        pxa_write_u16(data+4,d->audio_info.channel); pxa_write_u16(data+6,16);
        pxa_write_u32(data+8,d->pcm_size); pxa_write_u64(data+16,d->audio_pts); return 0;
    }
    if (op==PXA_VIDEO_IO_AUDIO_READ) {
        size=std::min(size,size_t(d->pcm_size-d->pcm_offset));
        if (size) std::memcpy(data,d->pcm+d->pcm_offset,size);
        d->pcm_offset+=size; return int32_t(size);
    }
    return PXA_STATUS_UNSUPPORTED;
}
const pxa_video_backend_t backend={sizeof(backend),nullptr,2,caps,open_decoder,decode,reset,destroy,audio_io};
}
void *pxa_video_malloc(size_t bytes) {
    // Macroblock state and small decoder controls are latency-sensitive.
    // Preserve internal headroom for TLS, audio and trusted UI; picture-sized
    // allocations and pressure fallbacks retain the app's PSRAM budget.
    if (internal_owner && bytes<=8192 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>bytes+80*1024) {
        if(void *p=pxa_memory_allocate(internal_owner,bytes))return p;
    }
    void *result=pxa_memory_allocate(allocation_owner,bytes);
    if(!result && allocation_fault)allocation_fault->store(PXA_STATUS_RESOURCE_LIMIT);
    return result;
}
void pxa_video_free(void *ptr) { pxa_memory_release(ptr); }
extern "C" const pxa_video_backend_t *pxa_video_codecs_backend(void) { return &backend; }

PxaVideoThreadAllocators pxa_video_thread_allocators() {return {allocation_owner,internal_owner,allocation_fault};}
void pxa_video_thread_allocators(PxaVideoThreadAllocators a) {
    allocation_fault=static_cast<std::atomic<int> *>(a.fault);
    allocation_owner=static_cast<const pxa_memory_allocator_t *>(a.external);
    internal_owner=static_cast<const pxa_memory_allocator_t *>(a.internal);
}

void pxa_video_worker_error() {
    if(allocation_fault) {int expected=0;allocation_fault->compare_exchange_strong(expected,PXA_STATUS_PROTOCOL_ERROR);}
}
