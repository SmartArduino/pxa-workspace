// SPDX-License-Identifier: MIT
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>

#include <driver/ppa.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_openh264_dec.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/usb_utmi_ll.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_default_config.h>

#include "esp_mosaico_usb_download.h"
#include "esp_mosaico_usb_restart.h"
#include "pxa_video_rgb565.hpp"

extern const uint8_t clip_0_start[] asm("_binary_clip_0_bin_start");
extern const uint8_t clip_0_end[] asm("_binary_clip_0_bin_end");
extern const uint8_t clip_1_start[] asm("_binary_clip_1_bin_start");
extern const uint8_t clip_1_end[] asm("_binary_clip_1_bin_end");

namespace {
enum class Output { yuv, packed_rgb, ppa_rgb };

struct Case {
    const char* name;
    esp_openh264_dec_mode_t mode;
    Output output;
};

constexpr Case cases[] = {
    {"fast_yuv", ESP_OPENH264_DEC_MODE_FAST, Output::yuv},
    {"fast_packed_rgb565", ESP_OPENH264_DEC_MODE_FAST, Output::packed_rgb},
    {"fast_ppa_rgb565", ESP_OPENH264_DEC_MODE_FAST, Output::ppa_rgb},
    {"max_speed_yuv", ESP_OPENH264_DEC_MODE_MAX_SPEED, Output::yuv},
};

struct Result {
    unsigned frames = 0;
    unsigned submitted = 0;
    uint64_t decoder_us = 0;
    uint64_t conversion_us = 0;
    uint64_t elapsed_us = 0;
    size_t min_sram = 0;
    size_t min_psram = 0;
    int error = 0;
};

constexpr auto sram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr auto psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr size_t packet_padding = 64;
constexpr uint32_t trace_magic = 0x34363248;
RTC_NOINIT_ATTR volatile struct {
    uint32_t signature;
    unsigned width;
    unsigned output;
    unsigned submitted;
} trace;

struct Decoder {
    esp_openh264_dec_handle_t handle = nullptr;
    ~Decoder() { if (handle) esp_openh264_dec_del(handle); }
};

struct OutputBuffers {
    uint8_t* packed_yuv = nullptr;
    uint8_t* rgb = nullptr;
    size_t rgb_capacity = 0;
    ppa_client_handle_t ppa = nullptr;
    ppa_srm_oper_config_t operation = {};

    ~OutputBuffers() {
        if (ppa) ppa_unregister_client(ppa);
        std::free(packed_yuv);
        std::free(rgb);
    }

    esp_err_t init(Output format, unsigned width, unsigned height) {
        if (format == Output::yuv) return ESP_OK;
        const size_t pixels = size_t(width) * height;
        rgb_capacity = (pixels * 2 + 127) & ~size_t(127);
        rgb = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, rgb_capacity, psram));
        if (!rgb) return ESP_ERR_NO_MEM;
        if (format != Output::ppa_rgb) return ESP_OK;
        const size_t yuv_capacity = (pixels * 3 / 2 + 127) & ~size_t(127);
        packed_yuv = static_cast<uint8_t*>(heap_caps_aligned_alloc(128, yuv_capacity, psram));
        if (!packed_yuv) return ESP_ERR_NO_MEM;
        ppa_client_config_t config = {};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
        operation.in.buffer = packed_yuv;
        operation.in.pic_w = operation.in.block_w = width;
        operation.in.pic_h = operation.in.block_h = height;
        operation.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
        operation.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
        operation.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        operation.out.buffer = rgb;
        operation.out.buffer_size = rgb_capacity;
        operation.out.pic_w = width;
        operation.out.pic_h = height;
        operation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        operation.scale_x = operation.scale_y = 1;
        operation.mode = PPA_TRANS_MODE_BLOCKING;
        return ppa_register_client(&config, &ppa);
    }

    esp_err_t convert(const esp_openh264_dec_frame_t& frame) {
        if (!pxa_video::pack_ppa420(frame.planes[0], frame.planes[1], frame.planes[2],
                frame.width, frame.height, frame.stride[0], frame.stride[1], packed_yuv, frame.width))
            return ESP_ERR_INVALID_ARG;
        return ppa_do_scale_rotate_mirror(ppa, &operation);
    }
};

struct Clip {
    std::span<const uint8_t> packets;
    unsigned width;
    unsigned height;
    unsigned count;
};

uint32_t read_u32(const uint8_t* bytes) {
    uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

Clip parse_clip(const uint8_t* begin, const uint8_t* end) {
    const size_t length = end - begin;
    if (length < 16 || std::memcmp(begin, "H264", 4) != 0) return {};
    return {{begin + 16, length - 16}, read_u32(begin + 4),
            read_u32(begin + 8), read_u32(begin + 12)};
}

int process_frame(Decoder& decoder, OutputBuffers& buffers, const Clip& clip, Output format,
        const uint8_t* packet, size_t size, Result& result) {
    const auto started = esp_timer_get_time();
    size_t written = 0;
    esp_openh264_dec_frame_t frame = {};
    esp_openh264_err_t status;
    if (format == Output::packed_rgb) {
        status = packet
            ? esp_openh264_dec_process(decoder.handle, packet, size, 0,
                  buffers.rgb, buffers.rgb_capacity, &written, nullptr)
            : esp_openh264_dec_flush(decoder.handle, buffers.rgb, buffers.rgb_capacity, &written, nullptr);
    } else {
        status = packet
            ? esp_openh264_dec_process_frame(decoder.handle, packet, size, 0, &frame, nullptr)
            : esp_openh264_dec_flush_frame(decoder.handle, &frame, nullptr);
    }
    const auto decoded_at = esp_timer_get_time();
    result.decoder_us += decoded_at - started;
    if (status != ESP_OPENH264_ERR_OK) return status;
    const bool valid = format == Output::packed_rgb
        ? written == size_t(clip.width) * clip.height * 2
        : frame.width == int(clip.width) && frame.height == int(clip.height) &&
          frame.planes[0] && frame.planes[1] && frame.planes[2];
    esp_err_t error = valid ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (valid && format == Output::ppa_rgb) {
        error = buffers.convert(frame);
        result.conversion_us += esp_timer_get_time() - decoded_at;
    }
    esp_openh264_dec_release_frame(frame.release_token);
    if (error == ESP_OK) ++result.frames;
    return error;
}

void decode(Decoder& decoder, OutputBuffers& buffers, const Clip& clip, Output format, Result& result) {
    auto packets = clip.packets;
    size_t largest = 0;
    for (unsigned index = 0; index < clip.count; ++index) {
        if (packets.size() < 4) { result.error = ESP_ERR_INVALID_SIZE; return; }
        const size_t size = read_u32(packets.data());
        packets = packets.subspan(4);
        if (!size || size > packets.size()) { result.error = ESP_ERR_INVALID_SIZE; return; }
        if (size > largest) largest = size;
        packets = packets.subspan(size);
    }
    if (!packets.empty()) { result.error = ESP_ERR_INVALID_SIZE; return; }
    auto input = std::unique_ptr<uint8_t, decltype(&std::free)>(
        static_cast<uint8_t*>(heap_caps_aligned_alloc(64, largest + packet_padding, psram)), std::free);
    if (!input) { result.error = ESP_ERR_NO_MEM; return; }

    packets = clip.packets;
    for (unsigned index = 0; index < clip.count; ++index) {
        const size_t size = read_u32(packets.data());
        packets = packets.subspan(4);
        trace.submitted = index;
        std::memcpy(input.get(), packets.data(), size);
        std::memset(input.get() + size, 0, packet_padding);
        const int status = process_frame(decoder, buffers, clip, format, input.get(), size, result);
        ++result.submitted;
        if (status != ESP_OPENH264_ERR_OK && status != ESP_OPENH264_ERR_AGAIN) {
            result.error = status;
            return;
        }
        packets = packets.subspan(size);
    }
    trace.submitted = clip.count;
    while (result.frames <= clip.count) {
        const int status = process_frame(decoder, buffers, clip, format, nullptr, 0, result);
        if (status == ESP_OPENH264_ERR_EOF && result.frames == clip.count) return;
        if (status != ESP_OPENH264_ERR_OK) { result.error = status; return; }
    }
    result.error = ESP_ERR_INVALID_SIZE;
}

Result bench(const Clip& clip, const Case& test) {
    Result result;
    if (!clip.count || !clip.width || !clip.height || clip.width > 1920 || clip.height > 1088) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    OutputBuffers buffers;
    result.error = buffers.init(test.output, clip.width, clip.height);
    if (result.error) return result;
    Decoder decoder;
    esp_openh264_dec_cfg_t config = ESP_OPENH264_DEC_CFG_DEFAULT();
    config.mode = test.mode;
    config.extra_dpb_slots = test.output == Output::packed_rgb ? 0 : 1;
    if (test.output == Output::packed_rgb) config.out_fmt = ESP_OPENH264_PIX_FMT_RGB565_LE;
    result.error = esp_openh264_dec_new(&config, &decoder.handle);
    if (!result.error) result.error = esp_openh264_dec_open(decoder.handle);
    if (result.error) return result;
    const bool monitoring = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    const auto started = esp_timer_get_time();
    decode(decoder, buffers, clip, test.output, result);
    result.elapsed_us = esp_timer_get_time() - started;
    if (monitoring) {
        result.min_sram = heap_caps_get_minimum_free_size(sram);
        result.min_psram = heap_caps_get_minimum_free_size(psram);
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    return result;
}

mosaico_board::UsbDownloadTrigger download_trigger;

void write_usb(const char* line) {
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, reinterpret_cast<const uint8_t*>(line), std::strlen(line));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(250));
}

void run() {
    const Clip clips[] = {parse_clip(clip_0_start, clip_0_end), parse_clip(clip_1_start, clip_1_end)};
    char line[460];
    std::snprintf(line, sizeof(line), "RAW_H264 version=%s cpu=%d psram=%d sram_free=%zu psram_free=%zu"
        " reset=%d prev_width=%u prev_output=%u prev_packet=%u\n",
        esp_openh264_dec_get_version(), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_SPIRAM_SPEED,
        heap_caps_get_free_size(sram), heap_caps_get_free_size(psram), int(esp_reset_reason()),
        trace.signature == trace_magic ? trace.width : 0,
        trace.signature == trace_magic ? trace.output : 0,
        trace.signature == trace_magic ? trace.submitted : 0);
    write_usb(line);
    for (const auto& clip : clips) {
        for (const auto& test : cases) {
            std::snprintf(line, sizeof(line), "RAW_H264 start res=%ux%u case=%s\n",
                clip.width, clip.height, test.name);
            write_usb(line);
            trace.width = clip.width;
            trace.output = static_cast<unsigned>(test.output);
            trace.submitted = 0;
            trace.signature = trace_magic;
            const auto result = bench(clip, test);
            trace.signature = 0;
            const double fps = result.elapsed_us ? result.frames * 1.0e6 / result.elapsed_us : 0;
            std::snprintf(line, sizeof(line), "RAW_H264 result res=%ux%u case=%s frames=%u packets=%u"
                " elapsed_us=%" PRIu64 " decoder_us=%" PRIu64 " conversion_us=%" PRIu64
                " fps=%.2f min_sram=%zu min_psram=%zu error=%d\n",
                clip.width, clip.height, test.name, result.frames, result.submitted,
                result.elapsed_us, result.decoder_us, result.conversion_us, fps,
                result.min_sram, result.min_psram, result.error);
            write_usb(line);
        }
    }
    write_usb("RAW_H264 done\n");
}
} // namespace

extern "C" void app_main() {
    static char serial[18];
    static const char language[] = {0x09, 0x04};
    static const char* strings[] = {language, "Espressif Systems", "ESP-Mosaico", serial, "H264 benchmark"};
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    std::snprintf(serial, sizeof(serial), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    usb.task.xCoreID = 0;
    usb.descriptor.string = strings;
    usb.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));

    static TaskHandle_t main_task = xTaskGetCurrentTaskHandle();
    tinyusb_config_cdcacm_t cdc = {};
    cdc.cdc_port = TINYUSB_CDC_ACM_0;
    cdc.callback_line_coding_changed = [](int, cdcacm_event_t* event) {
        download_trigger.SetBitRate(event->line_coding_changed_data.p_line_coding->bit_rate);
    };
    cdc.callback_line_state_changed = [](int, cdcacm_event_t* event) {
        if (download_trigger.SetDtr(event->line_state_changed_data.dtr))
            xTaskNotify(main_task, 1, eSetBits);
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc));
    bool download_requested = false;
    while (!download_requested) {
        uint32_t notification = 0;
        if (xTaskNotifyWait(0, 1, &notification, pdMS_TO_TICKS(20)) && notification)
            break;
        uint8_t command[32];
        size_t length = 0;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, command, sizeof(command), &length) != ESP_OK) continue;
        for (size_t index = 0; index < length; ++index) {
            if (command[index] == 'b') run();
            if (command[index] == 'r') download_requested = true;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_ERROR_CHECK(tinyusb_driver_uninstall());
    vTaskDelay(pdMS_TO_TICKS(100));
    usb_utmi_ll_enable_bus_clock(true);
    usb_utmi_ll_reset_register();
    EspMosaicoRestartToUsbDownload();
}
