// SPDX-License-Identifier: Apache-2.0
#include "pxa_video_codecs.h"
#include "pxa_video_rgb565.hpp"
#include "sdkconfig.h"
#ifndef PXA_VIDEO_NATIVE_SURFACE
#define PXA_VIDEO_NATIVE_SURFACE 0
#endif
#if PXA_USE_ESP_OPENH264
#include "esp_openh264_dec.h"
#if PXA_VIDEO_NATIVE_SURFACE
#include "pxa_video_native_surface.hpp"
#endif
#if CONFIG_IDF_TARGET_ESP32S31 || CONFIG_IDF_TARGET_ESP32P4
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#define PXA_ASYNC_OPENH264 1
#include "freertos/idf_additions.h"
#include "pxa/pxa_host.h"
#endif
#if CONFIG_LIBC_NEWLIB
#include <cstdio>
#undef stderr
// The vendor's Picolibc build refers to a global stderr pointer.
// Its trace calls use Newlib's initialized console stream on this board.
extern "C" {
FILE *stderr = &__sf[2];
}
#endif
#else
#include "pxa_video_memory.h"
#include "pxa_video_threads.h"
#include "wels/codec_api.h"
#endif
#if CONFIG_SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#include "esp_cache.h"
#include "pxa/pxa_esp_surface.h"
#endif
#if PXA_USE_ESP_OPENH264 && CONFIG_IDF_TARGET_ESP32P4 && CONFIG_SOC_PPA_SUPPORTED
#define PXA_P4_ASYNC_VIDEO_PPA 1
#else
#define PXA_P4_ASYNC_VIDEO_PPA 0
#endif
extern "C" void pxa_video_convert_rgb565(const uint8_t *,const uint8_t *,const uint8_t *,
    unsigned,unsigned,unsigned,unsigned,uint8_t *);
extern "C" void pxa_video_pack_half444(const uint8_t *,const uint8_t *,const uint8_t *,
    unsigned,unsigned,unsigned,unsigned,uint8_t *,unsigned);
extern "C" bool pxa_video_pack_ppa420(const uint8_t *,const uint8_t *,const uint8_t *,
    unsigned,unsigned,unsigned,unsigned,uint8_t *,unsigned);
#include "pxa_h264_limits.hpp"
#include "pxa_esp_resource_memory.h"
#include "esp_jpeg_dec.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "pxa_codec_memory.h"
#include "esp_aac_dec.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
#if !PXA_USE_ESP_OPENH264
thread_local const pxa_memory_allocator_t *allocation_owner,*internal_owner;
thread_local std::atomic<int> *allocation_fault;
struct Scope {
    const pxa_memory_allocator_t *previous,*previous_internal;
    std::atomic<int> *previous_fault;
    explicit Scope(const pxa_memory_allocator_t *owner,const pxa_memory_allocator_t *internal=nullptr,std::atomic<int> *fault=nullptr)
        : previous(allocation_owner),previous_internal(internal_owner),previous_fault(allocation_fault) {allocation_owner=owner;internal_owner=internal;allocation_fault=fault;}
    ~Scope() {allocation_owner=previous;internal_owner=previous_internal;allocation_fault=previous_fault;}
};
#endif
const pxa_video_capability_t caps[] = {
    {PXA_VIDEO_H264, PXA_VIDEO_SOFTWARE, 7, 640, 360, 1280, 1280, 1280*720, 512*1024, 5, pxa_video::h264_max_level, 1, 1},
    {PXA_VIDEO_MJPEG, PXA_VIDEO_SOFTWARE, 0, 640, 360, 1920, 1920, 1920*1080, 1024*1024, 25, 0, 2, 1},
};
struct Decoder {
    const pxa_memory_allocator_t *allocator = nullptr;
    const pxa_memory_allocator_t *audio_allocator = nullptr;
#if PXA_USE_ESP_OPENH264
    esp_openh264_dec_handle_t h264 = nullptr;
    bool packed_h264 = false;
    bool worker_pie = false;
#if PXA_ASYNC_OPENH264
    esp_openh264_dec_frame_t picture{};
    bool sample_planes = false;
#if CONFIG_SOC_PPA_SUPPORTED
    // The producer owns the RGB buffers for the duration of this binding.
    uint8_t *clean_borders[3]{};
#endif
#endif
#if PXA_ASYNC_OPENH264
    struct {
        TaskHandle_t worker = nullptr;
        SemaphoreHandle_t finished = nullptr;
        uint8_t *packet = nullptr;
        const uint8_t *input = nullptr;
        size_t capacity = 0, size = 0;
        uint64_t pts = 0;
        uint32_t flags = 0;
        bool active = false, planar = false, notify_host = false, surface_planes = false;
        pxa_status_t result = PXA_STATUS_OK;
        pxa_video_frame_t frame{};
    } async;
#endif
#else
    const pxa_memory_allocator_t *internal_allocator = nullptr;
    ISVCDecoder *h264 = nullptr;
    uint8_t *parameters = nullptr;
    size_t parameters_size = 0;
    int max_workers = PXA_VIDEO_H264_WORKERS, workers = 0;
    std::atomic<int> fault{0};
    bool seen_vcl = false;
    bool draining = false;
#endif
#if CONFIG_SOC_PPA_SUPPORTED
    uint8_t *yuv_storage = nullptr, *packed_yuv = nullptr;
    size_t yuv_capacity = 0;
#if PXA_P4_ASYNC_VIDEO_PPA
    uint64_t yuv_pack_us = 0, ppa_us = 0;
#endif
    ppa_client_handle_t ppa = nullptr;
    bool ppa_disabled = false;
#endif
    jpeg_dec_handle_t jpeg = nullptr;
    uint8_t *rgb = nullptr, *rgb_storage = nullptr;
    size_t rgb_capacity = 0;
#if PXA_P4_ASYNC_VIDEO_PPA
    struct {
        SemaphoreHandle_t complete = nullptr;
        uint8_t *rgb = nullptr, *storage = nullptr;
        size_t capacity = 0;
        pxa_video_frame_t picture{};
        bool configured = false;
        bool dma_active = false;
    } ppa_output;
    bool surface_planes_latched = false;
    struct {
        TaskHandle_t task = nullptr;
        SemaphoreHandle_t done = nullptr;
        void *token = nullptr;
        const uint8_t *planes[3]{};
        unsigned width = 0, height = 0, pitch = 0, ys = 0, cs = 0;
        unsigned submitted = 0;
        bool active = false;
    } packing;
#endif
    uint16_t width = 0, height = 0, index = 0;
    void *aac = nullptr;
    uint8_t *pcm = nullptr;
    esp_audio_dec_info_t audio_info{};
    uint32_t pcm_size = 0, pcm_offset = 0;
    uint64_t audio_pts = 0, codec_us = 0, conversion_us = 0;
    unsigned frames = 0;
};
#if PXA_USE_ESP_OPENH264
pxa_status_t h264_status(esp_openh264_err_t error) {
    switch (error) {
        case ESP_OPENH264_ERR_OK: return PXA_STATUS_OK;
        case ESP_OPENH264_ERR_MEM: return PXA_STATUS_RESOURCE_LIMIT;
        case ESP_OPENH264_ERR_UNSUPPORTED: return PXA_STATUS_UNSUPPORTED;
        default: return PXA_STATUS_PROTOCOL_ERROR;
    }
}
#endif
bool reserve_rgb(const pxa_memory_allocator_t *allocator, size_t bytes,
                 uint8_t *&storage, uint8_t *&pixels, size_t &capacity) {
    if (bytes <= capacity) return true;
    // Every decode invalidates the previous frame, including a failed decode.
    pxa_memory_release(storage);
    pixels = nullptr;
    capacity = 0;
    storage = static_cast<uint8_t*>(pxa_memory_allocate(allocator, bytes + 255));
    if (!storage) return false;
    pixels = reinterpret_cast<uint8_t*>((uintptr_t(storage) + 127) & ~uintptr_t(127));
    capacity = bytes;
    return true;
}
bool ensure_rgb(Decoder *d, size_t bytes) {
    return reserve_rgb(d->allocator, bytes, d->rgb_storage, d->rgb, d->rgb_capacity);
}
#if PXA_P4_ASYNC_VIDEO_PPA
bool IRAM_ATTR ppa_picture_done(ppa_client_handle_t, ppa_event_data_t *, void *context) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(context), &woken);
    return woken == pdTRUE;
}
void pack_worker(void *context);
void wait_ppa_output(Decoder *d) {
    if (d->packing.active) {
        xSemaphoreTake(d->packing.done, portMAX_DELAY);
        d->packing.active = false;
    }
    auto &output = d->ppa_output;
    if (!output.dma_active) return;
    const auto started = esp_timer_get_time();
    xSemaphoreTake(output.complete, portMAX_DELAY);
    d->ppa_us += esp_timer_get_time() - started;
    output.dma_active = false;
}
void enable_ppa_pipeline(Decoder *d) {
    auto &output = d->ppa_output;
    if (output.configured) return;
    output.configured = true;
    const size_t pixels = size_t(d->width) * d->height;
    // Bound the extra output buffer to the normal 480p playback budget.
    if (pixels > 854u * 480u) return;
    const ppa_event_callbacks_t callbacks = {.on_trans_done = ppa_picture_done};
    output.complete = xSemaphoreCreateBinary();
    if (output.complete &&
        reserve_rgb(d->allocator, pixels * 2, output.storage, output.rgb, output.capacity) &&
        ppa_client_register_event_callbacks(d->ppa, &callbacks) == ESP_OK) {
#if configNUMBER_OF_CORES > 1
        d->packing.done = xSemaphoreCreateBinary();
        if (d->packing.done &&
            xTaskCreatePinnedToCore(pack_worker, "pxa_yuv", 4096, d, 6, &d->packing.task, 1) != pdPASS) {
            vSemaphoreDelete(d->packing.done);
            d->packing.done = nullptr;
        }
#endif
        ESP_LOGI("PxaVideo", "PPA output overlaps the next H264 decode; packing worker=%d",
                 d->packing.task != nullptr);
        return;
    }
    if (output.complete) vSemaphoreDelete(output.complete);
    output.complete = nullptr;
    pxa_memory_release(output.storage);
    output.storage = output.rgb = nullptr;
    output.capacity = 0;
}
#endif
#if PXA_ASYNC_OPENH264
#if CONFIG_IDF_TARGET_ESP32S31
// A retained picture at this limit takes 384 KiB before DPB padding.
constexpr unsigned s31_overlap_pixels = 256u * 1024u;
#if CONFIG_SOC_PPA_SUPPORTED
// A whole 32-row PPA block preserves the four-bit scaling phase at each seam.
constexpr unsigned s31_strip_rows = 32;
bool prepare_ppa_input(Decoder *, unsigned, unsigned, unsigned &, bool);
#endif
#endif
#if CONFIG_SOC_PPA_SUPPORTED
// The SRM scale registers have four fractional bits. Derive both dimensions
// from one factor so the picture keeps its aspect ratio on rectangular panels.
bool preferred_size(void *context, uint16_t *width, uint16_t *height) {
    auto *d = static_cast<Decoder*>(context);
    if (d->index || (d->width & 1) || (d->height & 1)) return false;
#if CONFIG_IDF_TARGET_ESP32S31
    const uint32_t source_pixels = uint32_t(d->width)*d->height;
    if (source_pixels < 640u*360u || (*width >= d->width && *height >= d->height)) return false;
#else
    if (d->packed_h264) return false;
#endif
    const unsigned step = std::min({unsigned(*width)*16/d->width, unsigned(*height)*16/d->height, 255u});
    if (!step) return false;
    *width = d->width*step/16;
    *height = d->height*step/16;
    return *width && *height;
}
unsigned exact_ppa_step(unsigned sw, unsigned sh, unsigned dw, unsigned dh) {
    const unsigned step = std::max((dw*16+sw-1)/sw, (dh*16+sh-1)/sh);
    return step && step < 256 && sw*step/16 == dw && sh*step/16 == dh ? step : 0;
}
#endif
void set_surface_size(void *context, uint16_t width, uint16_t height) {
    auto *d = static_cast<Decoder*>(context);
#if CONFIG_SOC_PPA_SUPPORTED
    for (auto &buffer : d->clean_borders) buffer = nullptr;
#endif
    // The owner thread snapshots this choice when it submits each access unit.
#if PXA_P4_ASYNC_VIDEO_PPA
    d->sample_planes = !d->packed_h264 && width && height && exact_ppa_step(d->width,d->height,width,height);
#else
    const uint32_t source_pixels = uint32_t(d->width)*d->height;
    // Exact hardware downscales can consume the decoder planes directly.
    // Deep software downscales also avoid the full-size RGB intermediate.
#if CONFIG_SOC_PPA_SUPPORTED
    const unsigned step = exact_ppa_step(d->width,d->height,width,height);
#else
    constexpr unsigned step = 0;
#endif
    d->sample_planes = width && height && source_pixels >= 640u*360u &&
        ((step && step < 16) || uint64_t(width)*height*3 <= source_pixels);
#if CONFIG_IDF_TARGET_ESP32S31 && CONFIG_SOC_PPA_SUPPORTED
    if (d->sample_planes && step && (!PXA_VIDEO_NATIVE_SURFACE || source_pixels > s31_overlap_pixels)) {
        // Reserve the small shared scratch before the first IDR allocates its DPB.
        unsigned pitch;
        prepare_ppa_input(d,d->width,std::min(unsigned(d->height),s31_strip_rows),pitch,false);
    }
#endif
#endif
}

void release_picture(Decoder *d) {
    esp_openh264_dec_release_frame(d->picture.release_token);
    d->picture = {};
}

void render_picture(void *context, uint8_t *out, uint32_t w, uint32_t h, uint32_t stride) {
    const auto &p = static_cast<Decoder*>(context)->picture;
    pxa_video::scale_yuv420_rgb565(p.planes[0],p.planes[1],p.planes[2],
        p.width,p.height,p.stride[0],p.stride[1],out,w,h,stride);
}

pxa_status_t materialize_picture(void *context, pxa_video_frame_t *frame) {
    auto *d = static_cast<Decoder*>(context);
    if (!ensure_rgb(d,frame->size)) return PXA_STATUS_RESOURCE_LIMIT;
    const auto &p = d->picture;
    pxa_video_convert_rgb565(p.planes[0],p.planes[1],p.planes[2],p.width,p.height,p.stride[0],p.stride[1],d->rgb);
    frame->rgb565 = d->rgb;
    return PXA_STATUS_OK;
}

#endif
#if PXA_ASYNC_OPENH264
constexpr uint32_t decode_requested = 1u, decode_stopped = 2u;

void discard_decode(Decoder *d) {
    if (!d->async.active) return;
    xSemaphoreTake(d->async.finished, portMAX_DELAY);
    d->async.active = false;
}
#endif
void destroy(void *value) {
    auto *d = static_cast<Decoder*>(value);
    if (!d) return;
#if PXA_ASYNC_OPENH264
    if (d->async.worker) {
        discard_decode(d);
        xTaskNotify(d->async.worker, decode_stopped, eSetBits);
        xSemaphoreTake(d->async.finished, portMAX_DELAY);
        vSemaphoreDelete(d->async.finished);
    }
    pxa_memory_release(d->async.packet);
#endif
#if PXA_P4_ASYNC_VIDEO_PPA
    wait_ppa_output(d);
    if (d->packing.task) {
        xTaskNotify(d->packing.task, 2, eSetBits);
        xSemaphoreTake(d->packing.done, portMAX_DELAY);
        vSemaphoreDelete(d->packing.done);
    }
    if (d->ppa_output.complete) vSemaphoreDelete(d->ppa_output.complete);
    pxa_memory_release(d->ppa_output.storage);
#endif
#if PXA_ASYNC_OPENH264
    release_picture(d);
#endif
#if PXA_USE_ESP_OPENH264
    if (d->h264) esp_openh264_dec_del(d->h264);
#else
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
    if (d->h264) { WelsDestroyDecoder(d->h264); }
    pxa_memory_release(d->parameters);
#endif
#if CONFIG_SOC_PPA_SUPPORTED
    if(d->ppa)ppa_unregister_client(d->ppa);
    pxa_memory_release(d->yuv_storage);
#endif
    if (d->jpeg) jpeg_dec_close(d->jpeg);
    PxaCodecMemoryScope audio_scope(d->audio_allocator);
    if (d->aac) esp_aac_dec_close(d->aac);
    pxa_memory_release(d->pcm);
    pxa_memory_release(d->rgb_storage);
    delete d;
}
#if PXA_ASYNC_OPENH264
bool ensure_decode_worker(Decoder *d);
#endif
pxa_status_t initialize_h264(Decoder *d) {
#if PXA_USE_ESP_OPENH264
    esp_openh264_dec_cfg_t config = ESP_OPENH264_DEC_CFG_DEFAULT();
#if PXA_ASYNC_OPENH264
    // Its second stage uses core 1. Keep every SIMD input and drain on the
    // same core-0 task, including headers supplied through synchronous IO.
    d->worker_pie = ensure_decode_worker(d);
    config.enable_pie = d->worker_pie;
#endif
#if CONFIG_IDF_TARGET_ESP32P4
    // The PPA conversion has lower cost on larger frames on this target.
    d->packed_h264 = ((d->width | d->height) & 7) == 0 &&
        uint32_t(d->width) * d->height <= 360u * 480u;
#else
    // Packed output accepts cropped dimensions on an eight-pixel boundary.
    d->packed_h264 = ((d->width | d->height) & 7) == 0;
#endif
    if (d->packed_h264) config.out_fmt = ESP_OPENH264_PIX_FMT_RGB565_LE;
#if CONFIG_IDF_TARGET_ESP32P4
    else config.extra_dpb_slots = 1; // PPA may still be reading the previous reference picture.
#elif CONFIG_IDF_TARGET_ESP32S31
    const uint32_t pixels = uint32_t(d->width)*d->height;
    if (pixels >= 640u*360u && pixels <= s31_overlap_pixels) config.extra_dpb_slots = 1;
#endif
    config.mode = PXA_ESP_OPENH264_DECODE_MODE;
    auto error = esp_openh264_dec_new(&config, &d->h264);
    if (error != ESP_OPENH264_ERR_OK) return h264_status(error);
    error = esp_openh264_dec_open(d->h264);
    if (error == ESP_OPENH264_ERR_OK)
        ESP_LOGI("PxaVideo", "H264 decoder esp_openh264 mode=%s pie=%d output=%s",
                 config.mode == ESP_OPENH264_DEC_MODE_MAX_SPEED ? "MAX_SPEED" : "FAST",
                 config.enable_pie, d->packed_h264 ? "packed-rgb565" : "strided-yuv420");
    return h264_status(error);
#else
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
#endif
}
pxa_status_t open_decoder(void *, uint16_t index, uint16_t width, uint16_t height, void **out) {
    *out = nullptr;
    if (index >= 2) return PXA_STATUS_UNSUPPORTED;
    auto *d = new(std::nothrow) Decoder;
    if (!d) return PXA_STATUS_RESOURCE_LIMIT;
    d->width=width; d->height=height; d->index=index;
    d->allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_IMAGE);
    if (!d->allocator) { delete d; return PXA_STATUS_RESOURCE_LIMIT; }
    d->audio_allocator=pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL,PXA_MEMORY_AUDIO);
#if !PXA_USE_ESP_OPENH264
    if(size_t(width)*height>640*360)d->max_workers=0;
    d->internal_allocator=pxa_esp_resource_allocator(PXA_MEMORY_INTERNAL,PXA_MEMORY_IMAGE);
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
#endif
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
#if CONFIG_SOC_PPA_SUPPORTED
struct PpaTarget {
    uint8_t *pixels;
    unsigned width, height, x, y;
    size_t bytes;
    bool half;
    float scale;
};
bool pack_and_submit(Decoder *d, const uint8_t *const *planes, unsigned width, unsigned height,
                     unsigned ys, unsigned cs, unsigned pitch, const PpaTarget *target = nullptr) {
#if PXA_P4_ASYNC_VIDEO_PPA
    const auto pack_started = esp_timer_get_time();
#endif
    const bool half = target && target->half;
    if (half) {
        width /= 2;
        height /= 2;
        pxa_video_pack_half444(planes[0],planes[1],planes[2],width,height,ys,cs,d->packed_yuv,pitch);
    } else if (!pxa_video_pack_ppa420(planes[0],planes[1],planes[2],width,height,ys,cs,d->packed_yuv,pitch)) return false;
#if PXA_P4_ASYNC_VIDEO_PPA
    const auto ppa_started = esp_timer_get_time();
    d->yuv_pack_us += ppa_started - pack_started;
#endif
    ppa_srm_oper_config_t operation{};
    operation.in.buffer=d->packed_yuv;operation.in.pic_w=pitch;operation.in.block_w=width;
    operation.in.pic_h=operation.in.block_h=height;
    operation.in.srm_cm=half ? PPA_SRM_COLOR_MODE_YUV444 : PPA_SRM_COLOR_MODE_YUV420;
    operation.in.yuv_range=PPA_COLOR_RANGE_LIMIT;operation.in.yuv_std=PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    operation.out.buffer=d->rgb;operation.out.buffer_size=(size_t(width)*height*2+127)&~size_t(127);
    operation.out.pic_w=width;operation.out.pic_h=height;operation.out.srm_cm=PPA_SRM_COLOR_MODE_RGB565;
    if (target) {
        operation.out.buffer=target->pixels;
        operation.out.buffer_size=target->bytes;
        operation.out.pic_w=target->width;
        operation.out.pic_h=target->height;
        operation.out.block_offset_x=target->x;
        operation.out.block_offset_y=target->y;
    }
    operation.scale_x=operation.scale_y=target ? target->scale : 1.0f;
    operation.mode=PPA_TRANS_MODE_BLOCKING;
#if PXA_P4_ASYNC_VIDEO_PPA
    if (!target && d->ppa_output.complete) {
        operation.mode = PPA_TRANS_MODE_NON_BLOCKING;
        operation.user_data = d->ppa_output.complete;
    }
#endif
    const auto result = ppa_do_scale_rotate_mirror(d->ppa,&operation);
#if PXA_P4_ASYNC_VIDEO_PPA
    d->ppa_us += esp_timer_get_time() - ppa_started;
    if (!target) d->ppa_output.dma_active = result == ESP_OK && operation.mode == PPA_TRANS_MODE_NON_BLOCKING;
#endif
    if(result==ESP_OK)return true;
    d->ppa_disabled=true;ESP_LOGW("PxaVideo","PPA color conversion unavailable; using CPU conversion");
    return false;
}
#endif
#if PXA_P4_ASYNC_VIDEO_PPA
void pack_worker(void *context) {
    auto *d = static_cast<Decoder*>(context);
    auto &job = d->packing;
    for (;;) {
        uint32_t command = 0;
        xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);
        if (command & 2) break;
        if (!pack_and_submit(d, job.planes, job.width, job.height, job.ys, job.cs, job.pitch))
            pxa_video_convert_rgb565(job.planes[0], job.planes[1], job.planes[2],
                job.width, job.height, job.ys, job.cs, d->rgb);
        esp_openh264_dec_release_frame(job.token);
        xSemaphoreGive(job.done);
    }
    xSemaphoreGive(job.done);
    vTaskDelete(nullptr);
}
#endif
#if CONFIG_SOC_PPA_SUPPORTED
bool prepare_ppa_input(Decoder *d, unsigned width, unsigned height, unsigned &pitch, bool yuv444 = false) {
    if(d->ppa_disabled || (width&1) || (height&1))return false;
    if(!d->ppa) {
        ppa_client_config_t config{};config.oper_type=PPA_OPERATION_SRM;
        config.max_pending_trans_num=1;config.data_burst_length=PPA_DATA_BURST_LENGTH_128;
        if(ppa_register_client(&config,&d->ppa)!=ESP_OK){d->ppa_disabled=true;return false;}
    }
    // The DMA descriptor accepts an input pitch independent of its visible block.
    // Align packed rows for PIE without reading past the decoder's visible planes.
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    pitch = (width + 31u) & ~31u;
#else
    pitch = width;
#endif
    const size_t bytes = size_t(pitch) * height * 3 / (yuv444 ? 1 : 2);
    if(bytes>d->yuv_capacity) {
        pxa_memory_release(d->yuv_storage);d->yuv_storage=nullptr;d->packed_yuv=nullptr;d->yuv_capacity=0;
        d->yuv_storage=static_cast<uint8_t*>(pxa_memory_allocate(d->allocator,bytes+255));
        if(!d->yuv_storage)return false;
        d->packed_yuv=reinterpret_cast<uint8_t*>((uintptr_t(d->yuv_storage)+127)&~uintptr_t(127));
        if (pitch != width) std::memset(d->packed_yuv, 0, bytes);
        d->yuv_capacity=bytes;
    }
    return true;
}
#endif

bool convert_on_ppa(Decoder *d,const uint8_t *const *planes,unsigned width,unsigned height,unsigned ys,unsigned cs,void **token=nullptr) {
#if CONFIG_SOC_PPA_SUPPORTED
    unsigned pitch;
    if (!prepare_ppa_input(d, width, height, pitch)) return false;
#if PXA_P4_ASYNC_VIDEO_PPA
    enable_ppa_pipeline(d);
    auto &job = d->packing;
    // Only a retained DPB picture can outlive this decode call. The caller
    // waits for both the packing task and DMA before reusing either buffer.
    if (job.task && token && *token) {
        job.token = *token;
        *token = nullptr;
        std::copy_n(planes, 3, job.planes);
        job.width = width;
        job.height = height;
        job.ys = ys;
        job.cs = cs;
        job.pitch = pitch;
        job.active = true;
        ++job.submitted;
        xTaskNotify(job.task, 1, eSetBits);
        return true;
    }
#endif
    return pack_and_submit(d, planes, width, height, ys, cs, pitch);
#else
    return false;
#endif
}
#if PXA_ASYNC_OPENH264 && CONFIG_SOC_PPA_SUPPORTED
bool render_plane_surface(Decoder *d, const esp_openh264_dec_frame_t &p, uint8_t *base,
                          uint32_t stride, uint16_t rows, uint16_t x, uint16_t y,
                          uint16_t width, uint16_t height) {
    size_t bytes=size_t(stride)*rows;
    if (!base || !p.planes[0] || (stride & 1)) return false;
#if CONFIG_IDF_TARGET_ESP32S31
    // S31 can report a successful YUV444 transfer while delivering its three
    // components as RGB. Use the PPA's native YUV420 input and scale in SRM.
    constexpr bool half = false;
#else
    const bool half = p.width == 2 * width && p.height == 2 * height;
#endif
    unsigned step = 16;
    if (!half && (p.width != width || p.height != height)) {
        step = exact_ppa_step(p.width,p.height,width,height);
        if (!step) return false;
    }
#if PXA_VIDEO_NATIVE_SURFACE
    bool can_stage = !half;
#if CONFIG_IDF_TARGET_ESP32S31
    // Larger packed payloads per slot can crowd out the next H.264 keyframe.
    can_stage = uint32_t(p.width)*p.height <= s31_overlap_pixels;
#endif
#if PXA_GAMEKIT_VERIFY_NATIVE_OUTPUT
    can_stage = can_stage && pxa_gamekit_native_verified();
#endif
    if (can_stage && pxa_video::native_surface::stage(p,base,stride/2,rows,{x,y,width,height},step)) return true;
#endif
    // Native staging uses base only to identify the Surface slot. The eager
    // conversion below writes DMA output directly into that RGB buffer.
    if (uintptr_t(base) & 127) return false;
    if (bytes & 127) {
        bytes = pxa_esp_surface_staging_capacity(base, bytes);
        if (!bytes) return false;
    }
    unsigned pitch;
#if CONFIG_IDF_TARGET_ESP32S31
    const unsigned input_rows = std::min(unsigned(p.height),s31_strip_rows);
#else
    const unsigned input_rows = half ? height : p.height;
#endif
    if (!prepare_ppa_input(d,half ? width : p.width,input_rows,pitch,half)) return false;
    // The driver invalidates complete output rows, including pixels outside
    // the video rectangle. Publish the border once per buffer after each bind.
    const bool whole_lines = uint32_t(width)*2==stride &&
        !((uintptr_t(base+size_t(y)*stride) | (size_t(height)*stride)) & 127);
    if (!whole_lines) {
        bool known = false;
        uint8_t **empty = nullptr;
        for (auto &buffer : d->clean_borders) {
            if (buffer == base) { known = true; break; }
            if (!buffer) empty = &buffer;
        }
        if (!known) {
            if (esp_cache_msync(base,bytes,ESP_CACHE_MSYNC_FLAG_DIR_C2M)!=ESP_OK) return false;
            if (empty) *empty = base;
        }
    }
    const PpaTarget target{base,stride/2,rows,x,y,bytes,half,step / 16.0f};
#if CONFIG_IDF_TARGET_ESP32S31
    for (unsigned row = 0; row < unsigned(p.height); row += input_rows) {
        const unsigned count = std::min(input_rows,unsigned(p.height)-row);
        if (count*step < 16) break; // The final partial block produces no output row.
        const uint8_t *planes[] = {p.planes[0]+size_t(row)*p.stride[0],
            p.planes[1]+size_t(row/2)*p.stride[1],p.planes[2]+size_t(row/2)*p.stride[2]};
        auto strip = target;
        strip.y += row*step/16;
        if (!pack_and_submit(d,planes,p.width,count,p.stride[0],p.stride[1],pitch,&strip)) return false;
    }
#else
    if (!pack_and_submit(d,p.planes,p.width,p.height,p.stride[0],p.stride[1],pitch,&target)) return false;
#endif
    if (++d->frames == 1)
        ESP_LOGI("PxaVideo","PPA Surface output: %ux%u at %u,%u stride=%lu",
                 unsigned(width),unsigned(height),unsigned(x),unsigned(y),(unsigned long)stride);
    return true;
}
bool render_surface(void *context, uint8_t *base, uint32_t stride, uint16_t rows,
                    uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    auto *d = static_cast<Decoder*>(context);
    return render_plane_surface(d,d->picture,base,stride,rows,x,y,width,height);
}
#endif
#if PXA_USE_ESP_OPENH264
pxa_status_t decode_planar_h264(Decoder *d, const uint8_t *data, size_t size, uint64_t pts,
    uint32_t flags, pxa_video_frame_t *frame, bool surface_planes) {
    const auto started = esp_timer_get_time();
    if (!flags) {
        const auto status = pxa_video::validate_annexb(data, size, d->width, d->height);
        if (status) return status;
    }
#if PXA_ASYNC_OPENH264
    release_picture(d);
#endif
    struct Picture : esp_openh264_dec_frame_t {
        Picture() : esp_openh264_dec_frame_t{} {}
        ~Picture() { esp_openh264_dec_release_frame(release_token); }
    } picture;
    uint64_t output_pts = 0;
    const auto error = flags ? esp_openh264_dec_flush_frame(d->h264, &picture, &output_pts)
        : esp_openh264_dec_process_frame(d->h264, data, size, pts, &picture, &output_pts);
    const auto converted_at=esp_timer_get_time();
#if PXA_P4_ASYNC_VIDEO_PPA
    auto &output = d->ppa_output;
    // Pack the new picture only after the previous DMA has finished reading
    // the shared YUV staging buffer. H.264 itself can run during that DMA.
    if (output.configured) wait_ppa_output(d);
    if (output.picture.rgb565) {
        *frame = output.picture;
        output.picture = {};
        std::swap(d->rgb, output.rgb);
        std::swap(d->rgb_storage, output.storage);
        std::swap(d->rgb_capacity, output.capacity);
    }
#endif
    if (error == ESP_OPENH264_ERR_AGAIN || error == ESP_OPENH264_ERR_EOF) return PXA_STATUS_OK;
    if (error != ESP_OPENH264_ERR_OK) return h264_status(error);
    const auto width=picture.width, height=picture.height;
    const auto ys=picture.stride[0], cs=picture.stride[1];
    if (width<=0 || height<=0 || width>d->width || height>d->height) return PXA_STATUS_LIMIT_EXCEEDED;
    if (!picture.planes[0] || !picture.planes[1] || !picture.planes[2] ||
        ys<width || cs<(width+1)/2 || cs!=picture.stride[2]) return PXA_STATUS_PROTOCOL_ERROR;
    const size_t bytes=size_t(width)*height*2;
#if PXA_ASYNC_OPENH264
#if PXA_P4_ASYNC_VIDEO_PPA
    // Once the RGB pipeline has queued a picture it must drain in order.
    // A session that starts with a native Surface can keep its YUV picture
    // until the next packet, even if the consumer later detaches or resizes.
    if (surface_planes && !d->ppa_output.configured) d->surface_planes_latched = true;
    if (d->surface_planes_latched)
#endif
    {
        d->picture = picture;
        picture.release_token = nullptr;
        *frame = {uint16_t(width), uint16_t(height), uint32_t(width)*2, output_pts,
                  uint64_t(converted_at-started), nullptr, bytes};
        return PXA_STATUS_OK;
    }
#endif
#if PXA_P4_ASYNC_VIDEO_PPA
    const uint64_t packed_us = d->yuv_pack_us, ppa_us = d->ppa_us;
#endif
    if (!ensure_rgb(d,bytes)) return PXA_STATUS_RESOURCE_LIMIT;
    if (!convert_on_ppa(d,picture.planes,width,height,ys,cs,&picture.release_token))
        pxa_video_convert_rgb565(picture.planes[0],picture.planes[1],picture.planes[2],width,height,ys,cs,d->rgb);
    const auto ended=esp_timer_get_time();
    d->codec_us+=converted_at-started;
    d->conversion_us+=ended-converted_at;
    if (++d->frames%32==0) {
        ESP_LOGI("PxaVideo","frames=%u codec_ms=%llu conversion_ms=%llu",d->frames,
            (unsigned long long)(d->codec_us/1000),(unsigned long long)(d->conversion_us/1000));
#if PXA_P4_ASYNC_VIDEO_PPA
        ESP_LOGI("PxaVideo", "PPA stages: frames=%u pack_ms=%llu ppa_block_ms=%llu worker=%u", d->frames,
            (unsigned long long)(packed_us/1000), (unsigned long long)(ppa_us/1000), d->packing.submitted);
#endif
    }
    const pxa_video_frame_t converted = {
        uint16_t(width), uint16_t(height), uint32_t(width)*2, output_pts,
        uint64_t(ended-started), d->rgb, bytes};
#if PXA_P4_ASYNC_VIDEO_PPA
    if (output.complete) output.picture = converted;
    else
#endif
        *frame = converted;
    return PXA_STATUS_OK;
}

pxa_status_t decode_packed_h264(Decoder *d, const uint8_t *data, size_t size, uint64_t pts,
    uint32_t flags, pxa_video_frame_t *frame) {
#if PXA_ASYNC_OPENH264
    release_picture(d);
#endif
    const auto started = esp_timer_get_time();
    if (!flags) {
        const auto status = pxa_video::validate_annexb(data, size, d->width, d->height);
        if (status) return status;
    }
    const size_t max_bytes = size_t(d->width) * d->height * 2;
    size_t bytes = 0;
    uint64_t output_pts = 0;
    esp_openh264_err_t error;
    if (flags) {
        if (!ensure_rgb(d, max_bytes)) return PXA_STATUS_RESOURCE_LIMIT;
        error = esp_openh264_dec_flush(d->h264, d->rgb, d->rgb_capacity, &bytes, &output_pts);
    } else {
        // With a null output the vendor consumes parameter sets normally and
        // caches the first actual picture until the same AU is retried.
        error = esp_openh264_dec_process(d->h264, data, size, pts, d->rgb, d->rgb_capacity, &bytes, &output_pts);
        if (error == ESP_OPENH264_ERR_MEM && bytes > d->rgb_capacity) {
            if (bytes > max_bytes) return PXA_STATUS_LIMIT_EXCEEDED;
            if (!ensure_rgb(d, max_bytes)) return PXA_STATUS_RESOURCE_LIMIT;
            error = esp_openh264_dec_process(d->h264, data, size, pts, d->rgb, d->rgb_capacity, &bytes, &output_pts);
        }
    }
    if (error == ESP_OPENH264_ERR_AGAIN || error == ESP_OPENH264_ERR_EOF) return PXA_STATUS_OK;
    if (error != ESP_OPENH264_ERR_OK) {
        ESP_LOGW("PxaVideo", "esp_openh264 packed decode error=%d pts_ms=%llu",
                 error, (unsigned long long)(pts / 1000));
        return h264_status(error);
    }
    int32_t width = 0, height = 0;
    if (esp_openh264_dec_get_resolution(d->h264, &width, &height) != ESP_OPENH264_ERR_OK)
        return PXA_STATUS_PROTOCOL_ERROR;
    if (width <= 0 || height <= 0 || width > d->width || height > d->height)
        return PXA_STATUS_LIMIT_EXCEEDED;
    if (bytes != size_t(width) * height * 2) return PXA_STATUS_PROTOCOL_ERROR;
    const auto elapsed = uint64_t(esp_timer_get_time() - started);
    d->codec_us += elapsed;
    if (++d->frames % 32 == 0)
        ESP_LOGI("PxaVideo", "frames=%u packed_rgb_ms=%llu", d->frames,
                 (unsigned long long)(d->codec_us / 1000));
    *frame = {uint16_t(width), uint16_t(height), uint32_t(width) * 2, output_pts, elapsed, d->rgb, bytes};
    return PXA_STATUS_OK;
}
#endif
#if PXA_USE_ESP_OPENH264 && (CONFIG_IDF_TARGET_ESP32S31 || CONFIG_IDF_TARGET_ESP32P4)
// Keep the first decoder stage ahead of background services. Display and
// audio tasks retain priority over video.
constexpr UBaseType_t decode_priority = 5;
#endif

#if PXA_ASYNC_OPENH264
void decode_worker(void *value) {
    auto *d = static_cast<Decoder*>(value);
    auto &job = d->async;
    while (true) {
        uint32_t signal = 0;
        xTaskNotifyWait(0, UINT32_MAX, &signal, portMAX_DELAY);
        if (signal & decode_stopped) break;
        if (!(signal & decode_requested)) continue;
        const bool notify_host = job.notify_host;
        job.frame = {};
        job.result = job.planar ? decode_planar_h264(d, job.input, job.size, job.pts, job.flags, &job.frame, job.surface_planes)
                                : decode_packed_h264(d, job.input, job.size, job.pts, job.flags, &job.frame);
        if (job.result) ESP_LOGW("PxaVideo","H264 job failed: status=%d packet=%u",int(job.result),unsigned(job.size));
        xSemaphoreGive(job.finished);
        if (notify_host) pxa_host_io_notify();
    }
    xSemaphoreGive(job.finished);
    vTaskDeleteWithCaps(nullptr);
}

bool ensure_decode_worker(Decoder *d) {
    auto &job = d->async;
    if (job.worker) return true;
    job.finished = xSemaphoreCreateBinary();
    if (!job.finished) return false;
    constexpr auto stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    // The vendor's second H.264 stage is pinned to core 1.
    if (xTaskCreatePinnedToCoreWithCaps(decode_worker, "pxa_h264", 16384, d,
            decode_priority, &job.worker, 0, stack_caps) == pdPASS) return true;
    vSemaphoreDelete(job.finished);
    job.finished = nullptr;
    return false;
}

void start_decode_job(Decoder *d, const uint8_t *input, size_t size, uint64_t pts,
                      uint32_t flags, bool planar, bool notify_host) {
    auto &job = d->async;
    job.input = input;
    job.size = size;
    job.pts = pts;
    job.flags = flags;
    job.planar = planar;
    job.surface_planes = d->sample_planes;
    job.notify_host = notify_host;
    job.active = true;
    xTaskNotify(job.worker, decode_requested, eSetBits);
}
#endif

pxa_status_t decode(void *value, const uint8_t *data, size_t size, uint64_t pts,
    uint32_t flags, pxa_video_frame_t *frame) {
    auto *d=static_cast<Decoder*>(value);
#if PXA_USE_ESP_OPENH264
    if (!d->index) {
        bool surface_planes=false;
#if PXA_ASYNC_OPENH264
        surface_planes=d->sample_planes;
#endif
        const bool packed = d->packed_h264 && !surface_planes;
#if PXA_ASYNC_OPENH264
        // A synchronous caller keeps the input alive until completion. When it
        // shares core 1 with the vendor stage, submit the borrowed packet on
        // core 0 so the two stages can run concurrently.
        if (d->worker_pie || (xPortGetCoreID() == 1 && ensure_decode_worker(d))) {
            start_decode_job(d, data, size, pts, flags, !packed, false);
            discard_decode(d);
            *frame = d->async.frame;
            return d->async.result;
        }
        const auto previous = uxTaskPriorityGet(nullptr);
        if (previous < decode_priority) vTaskPrioritySet(nullptr, decode_priority);
#endif
        const auto status = packed ? decode_packed_h264(d, data, size, pts, flags, frame)
                                   : decode_planar_h264(d, data, size, pts, flags, frame, surface_planes);
#if CONFIG_IDF_TARGET_ESP32S31 || CONFIG_IDF_TARGET_ESP32P4
        if (previous < decode_priority) vTaskPrioritySet(nullptr, previous);
#endif
        return status;
    }
#else
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
    if(int fault=d->fault.exchange(0))return pxa_status_t(fault);
#endif
    const int64_t started=esp_timer_get_time();
    int32_t width=0,height=0;
    jpeg_dec_io_t jpeg{}; jpeg_dec_header_info_t header{};
#if !PXA_USE_ESP_OPENH264
    int32_t ys=0,cs=0;
    uint8_t *planes[3]{};
    if (!d->index) {
        SBufferInfo info{};
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
        ys=info.UsrData.sSystemBuffer.iStride[0];
        cs=info.UsrData.sSystemBuffer.iStride[1];
        pts=info.uiOutYuvTimeStamp;
    } else
#endif
    {
        if (flags) return PXA_STATUS_OK;
        jpeg.inbuf=const_cast<uint8_t*>(data); jpeg.inbuf_len=int(size);
        if (jpeg_dec_parse_header(d->jpeg,&jpeg,&header) != JPEG_ERR_OK) return PXA_STATUS_PROTOCOL_ERROR;
        width=header.width; height=header.height;
    }
    if (width<=0 || height<=0 || width>d->width || height>d->height) return PXA_STATUS_LIMIT_EXCEEDED;
    const int64_t conversion_started=esp_timer_get_time();
    d->codec_us+=conversion_started-started;
    size_t bytes=size_t(width)*height*2;
    if (!ensure_rgb(d, bytes)) return PXA_STATUS_RESOURCE_LIMIT;
#if !PXA_USE_ESP_OPENH264
    if (!d->index) {
        if (!planes[0] || !planes[1] || !planes[2]) return PXA_STATUS_PROTOCOL_ERROR;
        if (ys<int(width) || cs<int((width+1)/2)) return PXA_STATUS_PROTOCOL_ERROR;
        if(!convert_on_ppa(d,planes,width,height,ys,cs))
            pxa_video_convert_rgb565(planes[0],planes[1],planes[2],width,height,ys,cs,d->rgb);
    } else
#endif
    {
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
    *frame={uint16_t(width),uint16_t(height),uint32_t(width)*2,pts,uint64_t(ended-started),d->rgb,bytes};
    return PXA_STATUS_OK;
}
#if PXA_ASYNC_OPENH264
pxa_status_t queue_decode(void *value, const uint8_t *data, size_t size,
                          uint64_t pts, uint32_t flags) {
    auto *d = static_cast<Decoder*>(value);
    if (d->index) return PXA_STATUS_UNSUPPORTED;
    auto &job = d->async;
    if (job.active) return PXA_STATUS_BUSY;
    bool planar = !d->packed_h264;
#if CONFIG_IDF_TARGET_ESP32S31
    planar |= d->sample_planes;
#endif
    if (!planar && !ensure_rgb(d, size_t(d->width) * d->height * 2)) return PXA_STATUS_RESOURCE_LIMIT;
    constexpr size_t input_padding = 64;
    if (size + input_padding > job.capacity) {
        const size_t needed = size + input_padding;
        const size_t initial = std::clamp(size_t(d->width)*d->height/4, size_t(16384), size_t(131072));
        size_t capacity = std::max(needed, initial + input_padding);
        // The previous job has completed. Keeping its compressed bytes while
        // growing the buffer doubles the peak reservation on large keyframes.
        pxa_memory_release(job.packet);
        job.packet = static_cast<uint8_t*>(pxa_memory_allocate(d->allocator, capacity));
        if (!job.packet && capacity != needed) {
            capacity = needed;
            job.packet = static_cast<uint8_t*>(pxa_memory_allocate(d->allocator, capacity));
        }
        if (!job.packet) {
            job.capacity = 0;
            return PXA_STATUS_RESOURCE_LIMIT;
        }
        job.capacity = capacity;
    }
    if (!ensure_decode_worker(d)) return PXA_STATUS_RESOURCE_LIMIT;
    if (size) std::memcpy(job.packet, data, size);
    std::memset(job.packet + size, 0, input_padding);
    start_decode_job(d, job.packet, size, pts, flags, planar, true);
    return PXA_STATUS_OK;
}

#if CONFIG_SOC_PPA_SUPPORTED
pxa_status_t prepare_and_queue(void *context, const uint8_t *au, size_t bytes, uint64_t pts,
    uint32_t flags, uint8_t *pixels, uint32_t stride, uint16_t rows, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height) {
    auto *d = static_cast<Decoder*>(context);
    if (!d->sample_planes) return PXA_STATUS_UNSUPPORTED;
#if CONFIG_IDF_TARGET_ESP32S31
    if (uint32_t(d->width)*d->height > s31_overlap_pixels) return PXA_STATUS_UNSUPPORTED;
#endif
    if (!d->picture.release_token) return PXA_STATUS_WOULD_BLOCK;
#if PXA_P4_ASYNC_VIDEO_PPA
    if (!d->surface_planes_latched || d->ppa_output.configured) return PXA_STATUS_UNSUPPORTED;
#endif
    // The retained DPB token is owned by this caller. Only the next picture
    // slot belongs to the decoder worker after queue_decode signals it.
    const auto previous = d->picture;
    d->picture = {};
    const auto status = queue_decode(d,au,bytes,pts,flags);
    if (status) {
        d->picture = previous;
        return status;
    }
    if (!render_plane_surface(d,previous,pixels,stride,rows,x,y,width,height))
        pxa_video::scale_yuv420_rgb565(previous.planes[0],previous.planes[1],previous.planes[2],
            previous.width,previous.height,previous.stride[0],previous.stride[1],
            pixels+size_t(y)*stride+x*2,width,height,stride);
    esp_openh264_dec_release_frame(previous.release_token);
    return PXA_STATUS_OK;
}
#endif
pxa_status_t poll_decode(void *value, pxa_video_frame_t *frame) {
    auto &job = static_cast<Decoder*>(value)->async;
    if (!job.active) return PXA_STATUS_NOT_FOUND;
    if (xSemaphoreTake(job.finished, 0) != pdTRUE) return PXA_STATUS_WOULD_BLOCK;
    job.active = false;
    *frame = job.frame;
    return job.result;
}
#endif

pxa_status_t reset(void *value) {
    auto *d=static_cast<Decoder*>(value);
#if PXA_ASYNC_OPENH264
    discard_decode(d);
#endif
#if PXA_P4_ASYNC_VIDEO_PPA
    wait_ppa_output(d);
    d->ppa_output.picture = {};
#endif
#if PXA_ASYNC_OPENH264
    release_picture(d);
#endif
    PxaCodecMemoryScope audio_scope(d->audio_allocator);
    d->pcm_size=d->pcm_offset=0;
    if (d->aac && esp_aac_dec_reset(d->aac)!=ESP_AUDIO_ERR_OK)
        return PXA_STATUS_PROTOCOL_ERROR;
#if PXA_USE_ESP_OPENH264
    if (d->h264) return h264_status(esp_openh264_dec_reset(d->h264));
#else
    Scope scope(d->allocator,d->internal_allocator,&d->fault);
    d->draining=false;
    d->seen_vcl=false;pxa_memory_release(d->parameters);d->parameters=nullptr;d->parameters_size=0;
    if (d->h264) { WelsDestroyDecoder(d->h264); d->h264=nullptr; d->fault.store(0); return initialize_h264(d); }
#endif
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
const pxa_video_backend_t backend={
    sizeof(backend),nullptr,2,caps,open_decoder,decode,reset,destroy,audio_io,
#if PXA_ASYNC_OPENH264
    queue_decode,poll_decode,
#else
    nullptr,nullptr,
#endif
#if PXA_ASYNC_OPENH264
    render_picture,materialize_picture,set_surface_size,
#else
    nullptr,nullptr,nullptr,
#endif
#if PXA_ASYNC_OPENH264 && CONFIG_SOC_PPA_SUPPORTED
    render_surface,
#else
    nullptr,
#endif
#if PXA_ASYNC_OPENH264 && CONFIG_SOC_PPA_SUPPORTED
    preferred_size,
#else
    nullptr,
#endif
#if PXA_ASYNC_OPENH264 && CONFIG_SOC_PPA_SUPPORTED
    prepare_and_queue,
#else
    nullptr,
#endif
};
}
#if !PXA_USE_ESP_OPENH264
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

PxaVideoThreadAllocators pxa_video_thread_allocators() {return {allocation_owner,internal_owner,allocation_fault};}
void pxa_video_thread_allocators(PxaVideoThreadAllocators a) {
    allocation_fault=static_cast<std::atomic<int> *>(a.fault);
    allocation_owner=static_cast<const pxa_memory_allocator_t *>(a.external);
    internal_owner=static_cast<const pxa_memory_allocator_t *>(a.internal);
}

void pxa_video_worker_error() {
    if(allocation_fault) {int expected=0;allocation_fault->compare_exchange_strong(expected,PXA_STATUS_PROTOCOL_ERROR);}
}
#endif
extern "C" const pxa_video_backend_t *pxa_video_codecs_backend(void) { return &backend; }
