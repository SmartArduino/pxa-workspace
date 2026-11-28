// SPDX-License-Identifier: Apache-2.0
#include "board_io.hpp"
#include "dash.hpp"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <strings.h>

#include <esp_aac_dec.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_openh264_dec.h>
#include <esp_private/esp_sys_event_panic.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <riscv/rvruntime-frames.h>

namespace {
using board_io::report;

constexpr char bvid[] = "BV1oEy5zaE8y";
constexpr char view_url[] = "https://api.bilibili.com/x/web-interface/view?bvid=BV1oEy5zaE8y";
constexpr char referer[] = "https://www.bilibili.com/video/BV1oEy5zaE8y/";
constexpr char user_agent[] = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/130.0.0.0 Safari/537.36";
constexpr auto external_ram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr auto internal_ram = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr size_t max_access_unit = 256 * 1024;
constexpr size_t video_group = 128 * 1024;
constexpr size_t audio_group = 128 * 1024;
constexpr EventBits_t clock_ready = BIT0, audio_done = BIT1, lcd_done = BIT2;
constexpr EventBits_t failed = BIT3, wifi_ready = BIT4, video_fetched = BIT5, audio_fetched = BIT6;
constexpr EventBits_t video_primed = BIT7, video_done = BIT8;
constexpr EventBits_t playback_done = audio_done | lcd_done | video_fetched | audio_fetched;

constexpr unsigned panic_marker = 0x4f4e4c4e;
RTC_NOINIT_ATTR volatile struct {
    unsigned marker, core, exception, pc, ra, cause, value;
} panic_record;

static esp_err_t IRAM_ATTR save_panic(void*, void*);
ESP_PANIC_HANDLER_REGISTER(save_panic, 20) {
    const auto* info = static_cast<esp_panic_ctx_t*>(ctx)->info;
    const auto* frame = static_cast<const RvExcFrame*>(info->frame);
    panic_record.core = info->core;
    panic_record.exception = info->exception;
    panic_record.pc = reinterpret_cast<uintptr_t>(info->addr);
    panic_record.ra = frame ? frame->ra : 0;
    panic_record.cause = frame ? frame->mcause : 0;
    panic_record.value = frame ? frame->mtval : 0;
    panic_record.marker = panic_marker;
    return ESP_OK;
}

int64_t now() { return esp_timer_get_time(); }
EventGroupHandle_t events;
std::atomic<unsigned> wifi_disconnects;

void wifi_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT) {
        xEventGroupSetBits(events, wifi_ready);
    } else {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            ++wifi_disconnects;
            xEventGroupClearBits(events, wifi_ready);
        }
        esp_wifi_connect();
    }
}

esp_err_t connect_wifi(wifi_config_t& config) {
    constexpr char tag[] = "native_wifi";
    ESP_RETURN_ON_ERROR(nvs_flash_init(), tag, "PHY calibration storage");
    ESP_RETURN_ON_ERROR(esp_netif_init(), tag, "network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), tag, "event loop");
    if (!esp_netif_create_default_wifi_sta()) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), tag, "Wi-Fi driver");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event, nullptr), tag, "start handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, nullptr), tag, "disconnect handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr), tag, "IP handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), tag, "Wi-Fi settings");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), tag, "station mode");
    const auto result = esp_wifi_set_config(WIFI_IF_STA, &config);
    std::memset(&config, 0, sizeof(config));
    if (result != ESP_OK) return result;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), tag, "Wi-Fi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), tag, "Wi-Fi power save");
    if (!(xEventGroupWaitBits(events, wifi_ready, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000)) & wifi_ready))
        return ESP_ERR_TIMEOUT;
    wifi_ap_record_t ap{};
    ESP_RETURN_ON_ERROR(esp_wifi_sta_get_ap_info(&ap), tag, "AP status");
    report("NATIVE wifi_connected ssid=%.*s rssi=%d channel=%u\n", 32, ap.ssid, ap.rssi, ap.primary);
    return ESP_OK;
}

class Http {
public:
    ~Http() { if (client_) esp_http_client_cleanup(client_); }

    bool fetch(const char* url, std::vector<uint8_t>& bytes, size_t limit,
               uint64_t start = 0, bool range = false) {
        if (!client_) {
            esp_http_client_config_t config{};
            config.url = url;
            config.user_agent = user_agent;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.disable_auto_redirect = true;
            config.timeout_ms = 8000;
            config.buffer_size = 4096;
            // Signed CDN URLs can reach the parser's 2 KiB bound.
            config.buffer_size_tx = 3072;
            config.event_handler = record_header;
            config.user_data = this;
            client_ = esp_http_client_init(&config);
            if (!client_) return false;
            esp_http_client_set_header(client_, "Referer", referer);
            esp_http_client_set_header(client_, "Accept", "*/*");
            esp_http_client_set_header(client_, "Accept-Encoding", "identity");
            esp_http_client_set_header(client_, "Connection", "close");
        } else if (esp_http_client_set_url(client_, url) != ESP_OK) {
            return false;
        }
        range_ok_ = false;
        if (range) {
            char header[96];
            std::snprintf(header, sizeof(header), "bytes=%" PRIu64 "-%" PRIu64, start, start + limit - 1);
            esp_http_client_set_header(client_, "Range", header);
        }
        const size_t prefix = range ? bytes.size() : 0;
        bytes.resize(prefix + limit);
        esp_http_client_set_timeout_ms(client_, 8000);
        const auto opened = esp_http_client_open(client_, 0);
        int64_t length = -1;
        if (opened == ESP_OK) {
            esp_http_client_set_timeout_ms(client_, 500);
            const auto deadline = now() + 12000000;
            do {
                length = esp_http_client_fetch_headers(client_);
            } while (length == -ESP_ERR_HTTP_EAGAIN && now() < deadline);
        }
        const int status = esp_http_client_get_status_code(client_);
        const bool matched = range
            ? status == 206 && range_ok_ && first_ == start && last_ == start + limit - 1
            : status == 200;
        size_t used = 0;
        if (length >= 0 && matched && uint64_t(length) <= limit) {
            auto deadline = now() + 12000000;
            while (used < limit && now() < deadline) {
                errno = 0;
                const int got = esp_http_client_read(client_,
                    reinterpret_cast<char*>(bytes.data() + prefix + used), std::min<size_t>(4096, limit - used));
                if (got == -ESP_ERR_HTTP_EAGAIN || (!got && errno == EAGAIN &&
                    !esp_http_client_is_complete_data_received(client_))) continue;
                if (got <= 0) break;
                used += size_t(got);
                deadline = now() + 12000000;
            }
            if (used && (range ? used == limit : esp_http_client_is_complete_data_received(client_))) {
                esp_http_client_close(client_);
                bytes.resize(prefix + used);
                return true;
            }
        }
        esp_http_client_close(client_);
        bytes.resize(range ? prefix + used : 0);
        report("NATIVE http_retry status=%d open=%d length=%lld bytes=%zu range=%d\n",
               status, int(opened), (long long)length, used, range);
        return false;
    }

private:
    static esp_err_t record_header(esp_http_client_event_t* event) {
        if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key, "Content-Range")) {
            auto& http = *static_cast<Http*>(event->user_data);
            http.range_ok_ = std::sscanf(event->header_value, "bytes %" SCNu64 "-%" SCNu64,
                                        &http.first_, &http.last_) == 2;
        }
        return ESP_OK;
    }

    esp_http_client_handle_t client_ = nullptr;
    bool range_ok_ = false;
    uint64_t first_ = 0, last_ = 0;
};

struct Payload {
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;

    bili::Bytes sample(const bili::Sample& item) const {
        if (item.offset < offset || item.offset - offset > bytes.size() ||
            item.size > bytes.size() - (item.offset - offset)) return {};
        return {bytes.data() + size_t(item.offset - offset), item.size};
    }
};

struct Stream {
    explicit Stream(bool audio_track) : audio(audio_track) {}

    bool audio;
    bili::DashStream dash;
    bili::Track track;
    Payload buffers[2];
    QueueHandle_t free = nullptr, ready = nullptr;
    std::atomic<unsigned> bytes{0}, fetch_us{0}, ranges{0}, wait_us{0};

    bool prepare() {
        free = xQueueCreate(2, sizeof(Payload*));
        ready = xQueueCreate(2, sizeof(Payload*));
        if (!free || !ready) return false;
        for (auto& buffer : buffers) {
            auto* p = &buffer;
            xQueueSend(free, &p, 0);
        }
        return true;
    }
};

struct Frame {
    uint16_t* pixels = nullptr;
    uint64_t pts = 0;
};

struct Playback {
    Stream video{false}, audio{true};
    Frame frames[3];
    QueueHandle_t free = nullptr, ready = nullptr;
    esp_openh264_dec_handle_t decoder = nullptr;
    size_t frame_bytes = 0;
    uint64_t audio_pts = 0;
    int64_t clock_start = 0;
    unsigned video_inputs = 0, decoded = 0, h264_us = 0;
    unsigned lcd_frames = 0, lcd_us = 0, max_gap_us = 0;
    unsigned audio_count = 0, aac_us = 0, pcm_bytes = 0, nonzero = 0, pcm_gaps = 0;
    uint64_t last_pts = 0;
};

Playback play;

void fail(const char* stage, int code) {
    report("NATIVE error stage=%s code=%d sram=%zu psram=%zu\n", stage, code,
           heap_caps_get_free_size(internal_ram), heap_caps_get_free_size(external_ram));
    xEventGroupSetBits(events, failed);
}

bool cancelled() { return xEventGroupGetBits(events) & failed; }

template<class T>
T* take(QueueHandle_t queue) {
    T* item = nullptr;
    while (!cancelled()) {
        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) break;
    }
    return item;
}

template<class T>
bool send(QueueHandle_t queue, T* item) {
    while (!cancelled()) {
        if (xQueueSend(queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) return true;
    }
    return false;
}

bool get_range(Http& client, const bili::DashStream& stream, Payload& payload, uint64_t first, size_t size) {
    payload.offset = first;
    payload.bytes.clear();
    payload.bytes.reserve(size);
    const auto deadline = now() + 60000000;
    unsigned failures = 0;
    bool use_backup = false;
    while (payload.bytes.size() < size && !cancelled() && now() < deadline) {
        const auto before = payload.bytes.size();
        const auto limit = size - before;
        const auto& url = use_backup ? stream.backup_url : stream.url;
        if (client.fetch(url.c_str(), payload.bytes, limit, first + before, true)) {
            failures = 0;
            continue;
        }
        if (payload.bytes.size() > before) failures = 0;
        else if (++failures >= 4) return false;
        if (!stream.backup_url.empty()) use_backup = !use_backup;
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    return payload.bytes.size() == size;
}

bool resolve_streams() {
    Http client;
    Payload payload;
    auto fetch_api = [&client, &payload](const char* endpoint) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (client.fetch(endpoint, payload.bytes, 256 * 1024)) return true;
            vTaskDelay(pdMS_TO_TICKS(350));
        }
        return false;
    };
    if (!fetch_api(view_url)) return false;
    auto json = [&payload] { return media::json::Value{payload.bytes.data(), payload.bytes.size()}; };
    const uint64_t cid = json().get("data").get("cid").number();
    if (!cid || json().get("code").number(UINT64_MAX)) return false;
    char url[256];
    std::snprintf(url, sizeof(url),
        "https://api.bilibili.com/x/player/playurl?bvid=%s&cid=%" PRIu64 "&qn=16&fnval=16&fnver=0&fourk=0&platform=pc",
        bvid, cid);
    if (!fetch_api(url)) return false;
    if (json().get("code").number(UINT64_MAX) ||
        !bili::dash_tracks(json().get("data").get("dash"), 16, play.video.dash, play.audio.dash) ||
        play.audio.dash.url.empty()) return false;
    for (auto* stream : {&play.video, &play.audio}) {
        Http cdn;
        if (!get_range(cdn, stream->dash, payload, 0, stream->dash.init_bytes) ||
            !bili::parse_dash_init(payload.bytes, stream->dash, stream->track, !stream->audio))
            return false;
    }
    const auto& video = play.video.track;
    const auto& audio = play.audio.track;
    report("NATIVE stream bvid=%s cid=%" PRIu64 " qn=16 width=%u height=%u sample_rate=%u channels=%u"
           " video_segments=%zu audio_segments=%zu duration_ms=%" PRIu64 "\n",
           bvid, cid, video.width, video.height, unsigned(audio.rate), audio.channels,
           play.video.dash.segments.size(), play.audio.dash.segments.size(),
           play.video.dash.segments.back().end / 1000);
    return video.width == 360 && video.height == 480 && (audio.channels == 1 || audio.channels == 2);
}

bool fetch_media(Stream& stream) {
    Http client;
    const auto& dash = stream.dash;
    // Read consecutive DASH fragments into alternating buffers. Consumers
    // decode each group while the network task is fetching the next one.
    auto next = dash;
    for (next.next = 0; next.next < dash.segments.size() && !cancelled();) {
        auto* buffer = take<Payload>(stream.free);
        if (!buffer) return false;
        const auto range = bili::dash_range(next, stream.audio ? audio_group : video_group);
        const auto started = now();
        if (!get_range(client, dash, *buffer, range.offset, range.bytes)) {
            if (!cancelled()) fail(stream.audio ? "audio_download" : "video_download", ESP_FAIL);
            return false;
        }
        stream.bytes += range.bytes;
        stream.fetch_us += unsigned(now() - started);
        ++stream.ranges;
        if (!send(stream.ready, buffer)) return false;
        next.next = range.next;
    }
    return send<Payload>(stream.ready, nullptr);
}

void fetch_task(void* context) {
    auto& stream = *static_cast<Stream*>(context);
    fetch_media(stream);
    xEventGroupSetBits(events, stream.audio ? audio_fetched : video_fetched);
    vTaskDeleteWithCaps(nullptr);
}

Payload* consume(Stream& stream) {
    const auto started = now();
    auto* payload = take<Payload>(stream.ready);
    stream.wait_us += unsigned(now() - started);
    return payload;
}

void wait_until(int64_t deadline) {
    while (!cancelled()) {
        const auto remaining = deadline - now();
        if (remaining <= 0) return;
        vTaskDelay(pdMS_TO_TICKS(unsigned(std::min<int64_t>((remaining + 999) / 1000, 25))));
    }
}

void display_task(void*) {
    xEventGroupWaitBits(events, clock_ready | failed, pdFALSE, pdFALSE, portMAX_DELAY);
    const auto& track = play.video.track;
    int64_t previous = 0, window = now();
    unsigned last_frames = 0;
    while (auto* frame = take<Frame>(play.ready)) {
        wait_until(play.clock_start + int64_t(frame->pts) - int64_t(play.audio_pts));
        if (cancelled()) break;
        const auto started = now();
        const auto result = board_io::draw_rgb565(frame->pixels, track.width, track.height);
        if (result != ESP_OK) {
            fail("lcd", result);
            break;
        }
        const auto ended = now();
        ++play.lcd_frames;
        play.lcd_us += unsigned(ended - started);
        play.last_pts = frame->pts;
        if (previous) play.max_gap_us = std::max(play.max_gap_us, unsigned(ended - previous));
        previous = ended;
        if (!send(play.free, frame)) break;
        if (ended - window >= 5000000) {
            report("NATIVE display frames=%u fps=%.2f media_ms=%" PRIu64 " wall_ms=%lld late_ms=%lld\n",
                   play.lcd_frames, (play.lcd_frames - last_frames) * 1e6 / (ended - window),
                   play.last_pts / 1000, (long long)((ended - play.clock_start) / 1000),
                   (long long)((started - play.clock_start + int64_t(play.audio_pts) - int64_t(play.last_pts)) / 1000));
            last_frames = play.lcd_frames;
            window = ended;
        }
    }
    xEventGroupSetBits(events, lcd_done);
    vTaskDeleteWithCaps(nullptr);
}

void add_adts(const bili::Track& track, bili::Bytes sample, uint8_t* out) {
    const unsigned length = sample.size() + 7;
    const unsigned channels = track.channels;
    out[0] = 0xff;
    out[1] = 0xf1;
    out[2] = (1 << 6) | (track.aac_rate_index << 2) | (channels >> 2);
    out[3] = ((channels & 3) << 6) | (length >> 11);
    out[4] = length >> 3;
    out[5] = (length << 5) | 31;
    out[6] = 0xfc;
    std::memcpy(out + 7, sample.data(), sample.size());
}

bool render_audio(void* decoder) {
    auto& stream = play.audio;
    auto& track = stream.track;
    std::vector<uint8_t> adts(8192);
    std::vector<uint8_t> pcm(8192);
    int64_t previous = 0;
    while (auto* payload = consume(stream)) {
        if (!bili::parse_dash_range(payload->bytes, payload->offset, stream.dash, track)) {
            fail("aac_demux", ESP_ERR_INVALID_RESPONSE);
            return false;
        }
        for (const auto& item : track.samples) {
            if (cancelled()) return false;
            const auto sample = payload->sample(item);
            if (sample.empty() || sample.size() + 7 >= adts.size()) {
                fail("aac_sample", ESP_ERR_INVALID_SIZE);
                return false;
            }
            add_adts(track, sample, adts.data());
            esp_audio_dec_in_raw_t input{};
            input.buffer = adts.data();
            input.len = sample.size() + 7;
            esp_audio_dec_out_frame_t output{};
            output.buffer = pcm.data();
            output.len = pcm.size();
            esp_audio_dec_info_t format{};
            const auto started = now();
            const auto status = esp_aac_dec_decode(decoder, &input, &output, &format);
            play.aac_us += unsigned(now() - started);
            ++play.audio_count;
            if (status != ESP_AUDIO_ERR_OK || input.consumed != input.len ||
                format.sample_rate != track.rate || format.channel != track.channels ||
                format.bits_per_sample != 16 || !output.decoded_size ||
                output.decoded_size > pcm.size() || output.decoded_size % (2 * track.channels)) {
                fail("aac_decode", status);
                return false;
            }
            auto bytes = output.decoded_size;
            if (std::any_of(pcm.begin(), pcm.begin() + bytes, [](uint8_t v) { return v != 0; })) ++play.nonzero;
            if (track.channels == 1) {
                if (bytes > pcm.size() / 2) {
                    fail("aac_mono", ESP_ERR_INVALID_SIZE);
                    return false;
                }
                for (size_t i = bytes / sizeof(int16_t); i != 0; --i) {
                    pcm[4*i - 4] = pcm[4*i - 2] = pcm[2*i - 2];
                    pcm[4*i - 3] = pcm[4*i - 1] = pcm[2*i - 1];
                }
                bytes *= 2;
            }
            if (!play.pcm_bytes) {
                const auto ready = xEventGroupWaitBits(events, video_primed | failed, pdFALSE, pdFALSE,
                                                      pdMS_TO_TICKS(60000));
                if (!(ready & video_primed) || cancelled()) {
                    if (!cancelled()) fail("video_preroll", ESP_ERR_TIMEOUT);
                    return false;
                }
                play.audio_pts = item.pts;
                play.clock_start = now();
                xEventGroupSetBits(events, clock_ready);
            }
            const auto result = board_io::write_pcm(pcm.data(), bytes);
            if (result != ESP_OK) {
                fail("i2s_write", result);
                return false;
            }
            play.pcm_bytes += bytes;
            const auto submitted = now();
            if (previous && (submitted - previous) * track.rate > 6LL * 512 * 1000000)
                ++play.pcm_gaps;
            previous = submitted;
        }
        if (!send(stream.free, payload)) return false;
    }
    return !cancelled();
}

void audio_task(void*) {
    void* decoder = nullptr;
    const auto status = esp_aac_dec_open(nullptr, 0, &decoder);
    if (status == ESP_AUDIO_ERR_OK) {
        render_audio(decoder);
        esp_aac_dec_close(decoder);
    } else fail("aac_open", status);
    board_io::stop_audio();
    xEventGroupSetBits(events, audio_done);
    vTaskDeleteWithCaps(nullptr);
}

bool decode_picture(bili::Bytes annexb, uint64_t pts, bool& eof) {
    auto* frame = take<Frame>(play.free);
    if (!frame) return false;
    auto* output = reinterpret_cast<uint8_t*>(frame->pixels);
    size_t size = 0;
    uint64_t actual = 0;
    const auto started = now();
    const auto status = annexb.empty()
        ? esp_openh264_dec_flush(play.decoder, output, play.frame_bytes, &size, &actual)
        : esp_openh264_dec_process(play.decoder, annexb.data(), annexb.size(), pts,
                                  output, play.frame_bytes, &size, &actual);
    play.h264_us += unsigned(now() - started);
    if (status == ESP_OPENH264_ERR_OK && size == play.frame_bytes) {
        frame->pts = actual;
        ++play.decoded;
        if (!send(play.ready, frame)) return false;
        if (play.decoded == 1) xEventGroupSetBits(events, video_primed);
        return true;
    }
    if (status == ESP_OPENH264_ERR_AGAIN || status == ESP_OPENH264_ERR_EOF) {
        eof = status == ESP_OPENH264_ERR_EOF;
        return send(play.free, frame);
    }
    fail("h264_decode", status);
    return false;
}

bool render_video() {
    auto& stream = play.video;
    auto& track = stream.track;
    std::vector<uint8_t> packet;
    bool sequence_started = false, eof = false;
    while (auto* payload = consume(stream)) {
        if (!bili::parse_dash_range(payload->bytes, payload->offset, stream.dash, track)) {
            fail("avc_demux", ESP_ERR_INVALID_RESPONSE);
            return false;
        }
        for (const auto& item : track.samples) {
            if (!bili::annexb(payload->sample(item), track.nal_bytes, packet, max_access_unit)) {
                fail("avc_sample", ESP_ERR_INVALID_SIZE);
                return false;
            }
            if (!sequence_started) {
                packet.insert(packet.begin() + 16, track.parameters.begin(), track.parameters.end());
                sequence_started = true;
            }
            const size_t bytes = packet.size() - 16;
            packet.resize(packet.size() + 64, 0);
            ++play.video_inputs;
            if (!decode_picture({packet.data() + 16, bytes}, item.pts, eof)) return false;
        }
        if (!send(stream.free, payload)) return false;
    }
    while (!eof && !cancelled()) if (!decode_picture({}, 0, eof)) return false;
    return send<Frame>(play.ready, nullptr);
}

void video_task(void*) {
    render_video();
    xEventGroupSetBits(events, video_done);
    vTaskDeleteWithCaps(nullptr);
}

bool setup_playback() {
    const auto& video = play.video.track;
    play.frame_bytes = size_t(video.width) * video.height * 2;
    play.free = xQueueCreate(3, sizeof(Frame*));
    play.ready = xQueueCreate(3, sizeof(Frame*));
    if (!play.free || !play.ready || !play.audio.prepare() || !play.video.prepare()) return false;
    for (auto& frame : play.frames) {
        frame.pixels = static_cast<uint16_t*>(heap_caps_aligned_alloc(128, play.frame_bytes, external_ram));
        if (!frame.pixels) return false;
        auto* buffer = &frame;
        xQueueSend(play.free, &buffer, 0);
    }
    esp_openh264_dec_cfg_t config = ESP_OPENH264_DEC_CFG_DEFAULT();
    config.mode = ESP_OPENH264_DEC_MODE_FAST;
    config.out_fmt = ESP_OPENH264_PIX_FMT_RGB565_BE;
    return esp_openh264_dec_new(&config, &play.decoder) == ESP_OPENH264_ERR_OK &&
           esp_openh264_dec_open(play.decoder) == ESP_OPENH264_ERR_OK;
}

bool start_task(TaskFunction_t task, const char* name, uint32_t stack, void* context, UBaseType_t priority) {
    return xTaskCreatePinnedToCoreWithCaps(task, name, stack, context, priority, nullptr, 0, external_ram) == pdPASS;
}

void run(wifi_config_t& credentials) {
    // Let the radio reserve DMA descriptors before the LCD bus uses SRAM.
    auto result = connect_wifi(credentials);
    if (result != ESP_OK) return fail("wifi", result);
    result = board_io::init_board();
    if (result != ESP_OK) return fail("board_init", result);
    report("NATIVE lcd_ready\n");
    if (!resolve_streams()) return fail("bilibili_dash", ESP_ERR_INVALID_RESPONSE);
    result = board_io::init_audio(play.audio.track.rate);
    if (result != ESP_OK) return fail("audio_init", result);
    if (!setup_playback()) return fail("decoder_setup", ESP_ERR_NO_MEM);
    report("NATIVE start sram=%zu psram=%zu\n",
           heap_caps_get_free_size(internal_ram), heap_caps_get_free_size(external_ram));
    if (!start_task(fetch_task, "video_https", 16384, &play.video, 4) ||
        !start_task(fetch_task, "audio_https", 16384, &play.audio, 4) ||
        !start_task(display_task, "lcd_play", 6144, nullptr, 8) ||
        !start_task(audio_task, "aac_i2s", 12288, nullptr, 7) ||
        !start_task(video_task, "avc_decode", 16384, nullptr, 5)) {
        fail("tasks", ESP_ERR_NO_MEM);
        return;
    }
    const auto handled = xEventGroupWaitBits(events, video_done | failed, pdFALSE, pdFALSE, pdMS_TO_TICKS(360000));
    if (handled & failed) return;
    if (!(handled & video_done)) return fail("video_end", ESP_ERR_TIMEOUT);
    xEventGroupWaitBits(events, playback_done, pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));
    if ((xEventGroupGetBits(events) & playback_done) != playback_done)
        return fail("playback_end", ESP_ERR_TIMEOUT);
    const auto elapsed = now() - play.clock_start;
    report("NATIVE video_result frames=%u decoded=%u inputs=%u elapsed_us=%lld fps=%.2f"
           " h264_us=%u lcd_us=%u max_gap_us=%u last_pts_ms=%" PRIu64 " error=%d\n",
           play.lcd_frames, play.decoded, play.video_inputs, (long long)elapsed,
           play.lcd_frames * 1e6 / elapsed, play.h264_us, play.lcd_us, play.max_gap_us,
           play.last_pts / 1000, cancelled());
    report("NATIVE audio_result packets=%u nonzero=%u pcm_bytes=%u aac_us=%u pcm_gaps=%u rate=%u\n",
           play.audio_count, play.nonzero, play.pcm_bytes, play.aac_us, play.pcm_gaps, unsigned(play.audio.track.rate));
    report("NATIVE net_result video_bytes=%u audio_bytes=%u video_ranges=%u audio_ranges=%u"
           " video_fetch_us=%u audio_fetch_us=%u video_wait_us=%u audio_wait_us=%u disconnects=%u"
           " min_sram=%zu min_psram=%zu\n",
           play.video.bytes.load(), play.audio.bytes.load(), play.video.ranges.load(), play.audio.ranges.load(),
           play.video.fetch_us.load(), play.audio.fetch_us.load(), play.video.wait_us.load(), play.audio.wait_us.load(),
           wifi_disconnects.load(), heap_caps_get_minimum_free_size(internal_ram),
           heap_caps_get_minimum_free_size(external_ram));
    esp_openh264_dec_del(play.decoder);
    report("NATIVE done\n");
}
}  // namespace

extern "C" void app_main() {
    ESP_ERROR_CHECK(board_io::init_usb());
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_ERROR);
    events = xEventGroupCreate();
    if (!events) return;
    wifi_config_t credentials{};
    board_io::read_wifi(credentials);
#if CONFIG_ESP_OPENH264_PROFILE_PERFORMANCE
    constexpr char profile[] = "performance";
#else
    constexpr char profile[] = "minimal";
#endif
    report("NATIVE boot profile=%s reset=%d h264=%s\n", profile, int(esp_reset_reason()),
           esp_openh264_dec_get_version());
    if (panic_record.marker == panic_marker) {
        report("NATIVE previous_panic core=%u exception=%u pc=%08x ra=%08x cause=%08x value=%08x\n",
               panic_record.core, panic_record.exception, panic_record.pc, panic_record.ra,
               panic_record.cause, panic_record.value);
        panic_record.marker = 0;
    }
    run(credentials);
}
