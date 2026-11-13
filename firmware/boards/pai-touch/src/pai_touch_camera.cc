// SPDX-License-Identifier: Apache-2.0
#include "pai_touch_camera.h"
#include "pai_touch_camera_pixels.h"
#include "pai_touch_display_dma.h"
#include <pxa_board_common/vision_port.h>
#include <driver/i2c_master.h>
#include <esp_camera.h>
#include <esp_jpeg_enc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <algorithm>
#include <cstdint>
#if PXA_PAI_CAMERA_DIAGNOSTIC
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#endif

namespace {
constexpr char kTag[] = "PaiTouchCamera";
constexpr pxa_camera_profile_t kProfiles[] = {
    {1, PXA_VISION_JPEG, 160, 120, 55, PXA_VISION_MAX_JPEG, 66667, 0},
    {2, PXA_VISION_JPEG, 320, 240, 75, PXA_VISION_MAX_JPEG, 100000, PXA_CAMERA_PROFILE_DEFAULT},
    {3, PXA_VISION_JPEG, 640, 480, 75, PXA_VISION_MAX_JPEG, 250000, 0},
};
struct Camera {
    i2c_master_bus_handle_t bus = nullptr;
    jpeg_enc_handle_t encoder = nullptr;
    bool initialized = false;
    unsigned frames = 0;
    unsigned width = 0, height = 0;
    camera_fb_t* held = nullptr;
    bool preview_available = false;
    unsigned preview_frames = 0;
#if PXA_PAI_CAMERA_DIAGNOSTIC == 1
    uint8_t* diagnostic_pixels = nullptr;
#endif
} camera;

pxa_status_t PrepareProfile(void*, const pxa_camera_profile_t& profile) {
    // Touch owns I2C1. These GC2145 pins match the MZ42-DJJXJ-S3-6824 V3.0
    // schematic and the working PAI reference board.
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = GPIO_NUM_0;
    bus.scl_io_num = GPIO_NUM_4;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    auto error = i2c_new_master_bus(&bus, &camera.bus);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "SCCB bus: %s", esp_err_to_name(error));
        return PXA_STATUS_IO_ERROR;
    }
    camera_config_t config{};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = 8;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = I2C_NUM_0;
    config.pin_d0 = 13;
    config.pin_d1 = 15;
    config.pin_d2 = 16;
    config.pin_d3 = 14;
    config.pin_d4 = 12;
    config.pin_d5 = 10;
    config.pin_d6 = 9;
    config.pin_d7 = 7;
    config.pin_vsync = 5;
    config.pin_href = 6;
    config.pin_pclk = 11;
    // VGA needs a lower pixel clock for the whole-row SRAM DMA ring.
    config.xclk_freq_hz = profile.width >= 640 ? 16000000 : 32000000;
    // Backlight uses timer/channel 0, and power-off indication uses 1.
    config.ledc_timer = LEDC_TIMER_2;
    config.ledc_channel = LEDC_CHANNEL_2;
    config.pixel_format = PXA_PAI_CAMERA_DIAGNOSTIC == 1 ? PIXFORMAT_RGB565 : PIXFORMAT_YUV422;
    switch (profile.id) {
        case 1: config.frame_size = FRAMESIZE_QQVGA; break;
        case 2: config.frame_size = FRAMESIZE_QVGA; break;
        case 3: config.frame_size = FRAMESIZE_VGA; break;
        default: return PXA_STATUS_UNSUPPORTED;
    }
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    error = esp_camera_init(&config);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "GC2145 initialization: %s", esp_err_to_name(error));
        return PXA_STATUS_UNAVAILABLE;
    }
    camera.initialized = true;
    auto* sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != GC2145_PID) {
        ESP_LOGE(kTag, "Expected GC2145 sensor");
        return PXA_STATUS_UNSUPPORTED;
    }
    jpeg_enc_config_t encoder = DEFAULT_JPEG_ENC_CONFIG();
    camera.width = profile.width; camera.height = profile.height;
    encoder.width = camera.width;
    encoder.height = camera.height;
    encoder.src_type = PXA_PAI_CAMERA_DIAGNOSTIC == 1 ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_YCbYCr;
    encoder.quality = profile.quality;
    // Run inside the existing camera worker. No extra codec task or stack.
    encoder.task_enable = false;
    if (jpeg_enc_open(&encoder, &camera.encoder) != JPEG_ERR_OK)
        return PXA_STATUS_RESOURCE_LIMIT;
#if PXA_PAI_CAMERA_DIAGNOSTIC == 1
    camera.diagnostic_pixels = static_cast<uint8_t*>(heap_caps_malloc(camera.width*camera.height*2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!camera.diagnostic_pixels) return PXA_STATUS_RESOURCE_LIMIT;
#endif
    camera.frames = 0;
#if PXA_PAI_DISPLAY_DMA_TEST_FAULTS > 0
    PaiTouchDisplayDmaArmCameraTest();
#endif
    ESP_LOGI(kTag, "GC2145 ready: %ux%u YUYV422, JPEG quality=%u profile=%lu", camera.width, camera.height,
             profile.quality, static_cast<unsigned long>(profile.id));
    return PXA_STATUS_OK;
}
pxa_status_t Prepare(void* context, uint16_t format) {
    return format == PXA_VISION_JPEG ? PrepareProfile(context, kProfiles[1]) : PXA_STATUS_UNSUPPORTED;
}

camera_fb_t* Acquire() {
    if (camera.held) esp_camera_fb_return(camera.held);
    camera.held = esp_camera_fb_get();
    camera.preview_available = false;
    return camera.held;
}
bool ValidFrame(const camera_fb_t* frame) {
    return frame && frame->width == camera.width && frame->height == camera.height &&
        frame->format == (PXA_PAI_CAMERA_DIAGNOSTIC == 1 ? PIXFORMAT_RGB565 : PIXFORMAT_YUV422) && frame->len == camera.width*camera.height*2 &&
        !(reinterpret_cast<uintptr_t>(frame->buf) & 15);
}
void* AllocateImage(void*, size_t bytes) {
    return pxa_memory_allocate(pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_FRAME), bytes);
}
void ReleaseImage(void*, void* pointer) { pxa_memory_release(pointer); }
pxa_status_t Preview(void*, pxa_raster_asset_t** output) {
    *output=nullptr;
    const int64_t started=esp_timer_get_time();
    const auto* frame=camera.preview_available ? camera.held : Acquire();
    camera.preview_available=false;
    if (!frame) return PXA_STATUS_TIMED_OUT;
    if (!ValidFrame(frame)) return PXA_STATUS_PROTOCOL_ERROR;
    const int64_t acquired=esp_timer_get_time();
    const bool log_frame = camera.preview_frames % 60 == 0;
    const unsigned divisor=std::max(1u,(camera.width+319)/320);
    const unsigned width=camera.width/divisor,height=camera.height/divisor;
    pxa_asset_info_t info{};
    info.kind=PXA_ASSET_IMAGE; info.encoding=PXA_ASSET_ENCODING_RGB565;
    info.format_version=1; info.payload_offset=PXA_ASSET_FILE_HEADER_BYTES;
    info.width=width; info.height=height; info.decoded_bytes=width*height*2;
    info.stored_bytes=info.decoded_bytes+PXA_ASSET_FILE_HEADER_BYTES;
    uint8_t* pixels=nullptr;
    const auto status=pxa_asset_object_create(&info,AllocateImage,ReleaseImage,nullptr,output,&pixels);
    if (status!=PXA_STATUS_OK) return status;
    const int64_t allocated = log_frame ? esp_timer_get_time() : 0;
    auto* destination=reinterpret_cast<uint16_t*>(pixels);
    const auto* source=reinterpret_cast<const uint16_t*>(frame->buf);
    if (frame->format == PIXFORMAT_YUV422) {
        if (!PaiTouchYuyvPreview({frame->buf, frame->len}, camera.width, camera.height,
                divisor, {destination, size_t(width)*height})) {
            pxa_raster_asset_release(*output); *output=nullptr;
            return PXA_STATUS_PROTOCOL_ERROR;
        }
    } else if (divisor==1) {
        const auto* input=reinterpret_cast<const uint32_t*>(source);
        auto* result=reinterpret_cast<uint32_t*>(pixels);
        for (size_t i=0;i<info.decoded_bytes/4;++i) {
            const uint32_t value=input[i];
            result[i]=((value&0x00ff00ffu)<<8)|((value&0xff00ff00u)>>8);
        }
    } else {
        for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
            destination[y*width+x]=__builtin_bswap16(source[y*divisor*camera.width+x*divisor]);
    }
    const int64_t converted = log_frame ? esp_timer_get_time() : 0;
    pxa_raster_asset_finish_loading(*output);
    // The Core image owns its pixels. Queue one sensor frame while the app
    // presents this snapshot; WHEN_EMPTY stops DMA once that frame is ready.
    esp_camera_fb_return(camera.held);
    camera.held = nullptr;
    ++camera.preview_frames;
    if (log_frame) {
        const int64_t returned = esp_timer_get_time();
        ESP_LOGI(kTag,"RGB preview frame=%u acquire_us=%lld convert_us=%lld alloc_us=%lld pixels_us=%lld return_us=%lld",camera.preview_frames,
            acquired-started,returned-acquired,allocated-acquired,
            converted-allocated,returned-converted);
    }
    return PXA_STATUS_OK;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    *size = 0;
    if (!camera.initialized || !camera.encoder) return PXA_STATUS_BAD_STATE;
    const int64_t begin = esp_timer_get_time();
    auto* frame = Acquire();
    if (!frame) return PXA_STATUS_TIMED_OUT;
    const int64_t acquired=esp_timer_get_time();
    int encoded = 0;
    pxa_status_t result = PXA_STATUS_PROTOCOL_ERROR;
    if (ValidFrame(frame)) {
        const size_t input_bytes = frame->len;
#if PXA_PAI_CAMERA_DIAGNOSTIC
        static bool captured = false;
        if (!captured) {
            auto* file = std::fopen("/pxa/pxa-state/camera-probe.rgb", "wb");
            const auto stored = file ? std::fwrite(frame->buf, 1, frame->len, file) : 0;
            if (file) std::fclose(file);
            ESP_LOGW(kTag, "RAW probe: %ux%u stored=%u bytes=%u worker_stack_free=%u",
                unsigned(frame->width), unsigned(frame->height), unsigned(stored), unsigned(frame->len),
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
            captured = true;
        }
#if PXA_PAI_CAMERA_DIAGNOSTIC == 1
        constexpr uint16_t colors[] = {0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0};
        for (unsigned y=0; y<frame->height; ++y) for (unsigned x=0; x<frame->width; ++x) {
            const auto pixel=colors[x*8/frame->width];
            const auto offset=(y*frame->width+x)*2;
            camera.diagnostic_pixels[offset]=uint8_t(pixel>>8);
            camera.diagnostic_pixels[offset+1]=uint8_t(pixel);
        }
#endif
#endif
#if PXA_PAI_CAMERA_DIAGNOSTIC == 1
        const auto* input = camera.diagnostic_pixels;
#else
        const uint8_t* input = frame->buf;
#endif
        const auto error = jpeg_enc_process(camera.encoder, input, input_bytes,
                                           output, capacity, &encoded);
        result = error == JPEG_ERR_OK && encoded > 0 && size_t(encoded) <= capacity
            ? PXA_STATUS_OK : PXA_STATUS_IO_ERROR;
    }
#if PXA_PAI_CAMERA_DIAGNOSTIC
    if (camera.frames % 30 == 0) ESP_LOGW(kTag, "Encoder worker stack free=%u", unsigned(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    camera.preview_available=result==PXA_STATUS_OK && camera.held;
    if (result == PXA_STATUS_OK) *size = size_t(encoded);
    if (++camera.frames % 30 == 1)
        ESP_LOGI(kTag, "frame=%u bytes=%u acquire_us=%lld encode_us=%lld status=%ld",
                 camera.frames, unsigned(*size), acquired-begin, esp_timer_get_time()-acquired, long(result));
    return result;
}

void Finish(void*) {
    if (camera.held) esp_camera_fb_return(camera.held);
    camera.held=nullptr; camera.preview_available=false; camera.preview_frames=0;
#if PXA_PAI_CAMERA_DIAGNOSTIC == 1
    heap_caps_free(camera.diagnostic_pixels);
    camera.diagnostic_pixels = nullptr;
#endif
    if (camera.encoder) {
        (void)jpeg_enc_close(camera.encoder);
        camera.encoder = nullptr;
    }
    if (camera.initialized) {
        (void)esp_camera_deinit();
        camera.initialized = false;
    }
    if (camera.bus) {
        (void)i2c_del_master_bus(camera.bus);
        camera.bus = nullptr;
    }
}
}

bool PaiTouchInitializeCamera() {
    esp_log_level_set("camera", ESP_LOG_WARN);
    esp_log_level_set("cam_hal", ESP_LOG_WARN);
    esp_log_level_set("s3 ll_cam", ESP_LOG_WARN);
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish,
        kProfiles, sizeof(kProfiles) / sizeof(kProfiles[0]), PrepareProfile, Preview};
    return pxa_board_vision_port_initialize(&driver);
}
