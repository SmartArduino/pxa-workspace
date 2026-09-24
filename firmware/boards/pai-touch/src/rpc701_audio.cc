#include "rpc701_audio.h"

#include "pai_touch_config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <driver/uart.h>
#include <freertos/idf_additions.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_audio_dec_default.h>
#include <esp_gmf_audio_helper.h>
#include <esp_heap_caps.h>
#include <esp_event.h>
#include <esp_log.h>
#include <pxa/pxa_host.h>
#include <pxa/audio.h>
#include <rpc_701.h>
#include <rpc_adapter.h>
#include <rpc_wrap.h>

namespace {
constexpr char kTag[] = "Rpc701Audio";
constexpr size_t kRpcPcmSamples = 160;
constexpr size_t kDecoderInputBytes = 768;
constexpr size_t kDecoderOutputBytes = 16384;
constexpr size_t kConvertedSamples = 2048;
constexpr uint32_t kMusicTaskStackBytes = 20480;
}

bool Rpc701Audio::Initialize() {
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Cannot create event loop: %s", esp_err_to_name(result));
        return false;
    }

    output_queue_ = xQueueCreateWithCaps(
        12, sizeof(OutputFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    volume_queue_ = xQueueCreate(1, sizeof(uint8_t));
    if (output_queue_ == nullptr || volume_queue_ == nullptr) {
        ESP_LOGE(kTag, "Cannot allocate RPC701 audio queues");
        return false;
    }

    const rpc_adapter_config_t config = {
        .uart_num = PAI_JL701_UART,
        .tx_pin = PAI_JL701_TX,
        .rx_pin = PAI_JL701_RX,
        .baud_rate = PAI_JL701_BAUD,
        .configure_pa = true,
        .pa_io = PAI_JL701_PA_IO,
        .pa_en_level = PAI_JL701_PA_LEVEL,
    };
    rpc_adapter_init(&config);
    if (!rpc_adapter_is_initialized()) {
        ESP_LOGE(kTag, "RPC701 transport initialization failed");
        return false;
    }
    audio_opus_frame_flush();

    if (xTaskCreate(AudioTask, "rpc701_audio", 4096, this, 6, &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(kTag, "Cannot create RPC701 audio task");
        return false;
    }
    pxa_host_set_audio_sink(Submit, Flush, this);
    music_commands_ = xQueueCreate(1, sizeof(MusicCommand));
    sound_events_ = xQueueCreateWithCaps(
        16, sizeof(SoundEvent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    music_mutex_ = xSemaphoreCreateMutex();
    music_ring_ = static_cast<int16_t*>(heap_caps_malloc(
        kMusicSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (music_ring_ == nullptr) {
        music_ring_ = static_cast<int16_t*>(malloc(kMusicSamples * sizeof(int16_t)));
    }
    if (music_commands_ != nullptr && music_mutex_ != nullptr &&
        music_ring_ != nullptr &&
        esp_vorbis_dec_register() == ESP_AUDIO_ERR_OK &&
        esp_opus_dec_register() == ESP_AUDIO_ERR_OK &&
        esp_ogg_dec_register() == ESP_AUDIO_ERR_OK &&
        xTaskCreate(MusicTask, "rpc701_music", kMusicTaskStackBytes, this, 5,
                    &music_task_) == pdPASS) {
        pxa_host_set_audio_asset_sink(PlayAsset, ControlAsset, this);
    } else {
        ESP_LOGW(kTag, "Compressed music task unavailable; PCM effects remain active");
    }
    SetVolume(volume_.load());
    if (rpc_adapter_is_slave_ready()) {
        ESP_LOGI(kTag,
                 "RPC701 ready: input=Opus queue, output=16 kHz mono PCM, UART2=2 Mbps");
    } else {
        ESP_LOGW(kTag,
                 "RPC701 UART started; waiting for the slave handshake retry");
    }
    return true;
}

void Rpc701Audio::SetVolume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    volume_.store(percent);
    if (volume_queue_ != nullptr) xQueueOverwrite(volume_queue_, &percent);
}

size_t Rpc701Audio::ReadOpus(uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity == 0) return 0;
    const int size = audio_opus_frame_read(output, capacity);
    return size > 0 ? static_cast<size_t>(size) : 0;
}

bool Rpc701Audio::Submit(void* context, uint8_t voice, const int16_t* pcm,
                         size_t samples) {
    auto* self = static_cast<Rpc701Audio*>(context);
    if (self == nullptr || self->output_queue_ == nullptr || pcm == nullptr ||
        samples == 0 || samples > kFrameSamples) {
        return false;
    }
    OutputFrame frame = {};
    frame.samples = static_cast<uint16_t>(samples);
    std::memcpy(frame.pcm, pcm, samples * sizeof(*pcm));
    (void)voice;
    return xQueueSend(self->output_queue_, &frame, 0) == pdTRUE;
}

void Rpc701Audio::Flush(void* context, uint8_t voice) {
    auto* self = static_cast<Rpc701Audio*>(context);
    (void)voice;
    if (self != nullptr && self->output_queue_ != nullptr) {
        xQueueReset(self->output_queue_);
    }
}

bool Rpc701Audio::PlayAsset(void* context, uint8_t voice,
                             const char* absolute_path, bool loop,
                             int16_t gain_db_q8) {
    auto* self = static_cast<Rpc701Audio*>(context);
    if (self == nullptr || self->music_commands_ == nullptr ||
        absolute_path == nullptr) return false;
    const size_t path_length = strnlen(absolute_path, kMusicPathSize);
    if (path_length == 0 || path_length == kMusicPathSize) return false;
    if (path_length >= 4 &&
        strcmp(absolute_path + path_length - 4, ".pcm") == 0) {
        if (loop || self->sound_events_ == nullptr) return false;
        if (self->sound_cache_ == nullptr) {
            self->sound_cache_ = static_cast<SoundAsset*>(heap_caps_calloc(
                kSoundCacheEntries, sizeof(SoundAsset),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (self->sound_cache_ == nullptr) return false;
        }
        SoundAsset* asset = nullptr;
        for (size_t index = 0; index < self->sound_cache_count_; ++index) {
            if (strcmp(self->sound_cache_[index].path, absolute_path) == 0) {
                asset = &self->sound_cache_[index];
                break;
            }
        }
        if (asset == nullptr) {
            if (self->sound_cache_count_ == kSoundCacheEntries) return false;
            FILE* file = fopen(absolute_path, "rb");
            if (file == nullptr) return false;
            const bool valid = fseek(file, 0, SEEK_END) == 0;
            const long length = valid ? ftell(file) : -1;
            if (length <= 0 || length > 16000 || fseek(file, 0, SEEK_SET) != 0) {
                fclose(file);
                return false;
            }
            auto* samples = static_cast<uint8_t*>(heap_caps_malloc(
                static_cast<size_t>(length), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (samples == nullptr) {
                fclose(file);
                return false;
            }
            const bool loaded = fread(samples, 1, static_cast<size_t>(length),
                                      file) == static_cast<size_t>(length);
            fclose(file);
            if (!loaded) {
                free(samples);
                return false;
            }
            asset = &self->sound_cache_[self->sound_cache_count_++];
            memcpy(asset->path, absolute_path, path_length + 1);
            asset->pcm = samples;
            asset->samples = static_cast<uint32_t>(length);
        }
        const SoundEvent event = {
            asset->pcm, asset->samples,
            static_cast<int32_t>(32768.0f *
                powf(10.0f, gain_db_q8 / (20.0f * 256.0f)))
        };
        return xQueueSend(self->sound_events_, &event, 0) == pdTRUE;
    }
    uint32_t format = 0;
    if (esp_gmf_audio_helper_get_audio_type_by_uri(absolute_path, &format) !=
            ESP_GMF_ERR_OK || format != ESP_FOURCC_OGG) return false;
    const int owner = self->music_voice_.load();
    if (owner >= 0 && owner != voice) return false;
    MusicCommand command = {};
    command.loop = loop;
    memcpy(command.path, absolute_path, path_length + 1);
    command.token = self->music_token_.fetch_add(1) + 1;
    self->music_voice_.store(voice);
    self->music_gain_q15_.store(static_cast<int32_t>(32768.0f *
        powf(10.0f, gain_db_q8 / (20.0f * 256.0f))));
    self->music_paused_.store(false);
    self->music_active_.store(true);
    return xQueueOverwrite(self->music_commands_, &command) == pdTRUE;
}

bool Rpc701Audio::ControlAsset(void* context, uint8_t voice,
                                uint8_t action, int16_t gain_db_q8) {
    auto* self = static_cast<Rpc701Audio*>(context);
    if (self == nullptr || self->music_task_ == nullptr ||
        self->music_voice_.load() != voice) return false;
    switch (action) {
        case PXA_AUDIO_ASSET_PAUSE:
            self->music_paused_.store(true);
            return true;
        case PXA_AUDIO_ASSET_RESUME:
            self->music_paused_.store(false);
            return true;
        case PXA_AUDIO_ASSET_STOP:
            self->music_active_.store(false);
            self->music_voice_.store(-1);
            self->music_token_.fetch_add(1);
            return true;
        case PXA_AUDIO_ASSET_SET_GAIN:
            self->music_gain_q15_.store(static_cast<int32_t>(32768.0f *
                powf(10.0f, gain_db_q8 / (20.0f * 256.0f))));
            return true;
        default:
            return false;
    }
}

void Rpc701Audio::AudioTask(void* context) {
    static_cast<Rpc701Audio*>(context)->Run();
    vTaskDelete(nullptr);
}

void Rpc701Audio::MusicTask(void* context) {
    static_cast<Rpc701Audio*>(context)->RunMusic();
    vTaskDelete(nullptr);
}

bool Rpc701Audio::QueueMusic(const int16_t* samples, size_t count,
                              uint32_t token) {
    size_t copied = 0;
    while (copied < count && music_token_.load() == token) {
        if (xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        const size_t available = std::min(count - copied,
                                           kMusicSamples - music_count_);
        for (size_t index = 0; index < available; ++index) {
            const size_t position = (music_read_ + music_count_) % kMusicSamples;
            music_ring_[position] = samples[copied + index];
            ++music_count_;
        }
        xSemaphoreGive(music_mutex_);
        copied += available;
        if (available == 0) vTaskDelay(1);
    }
    return copied == count;
}

void Rpc701Audio::RunMusic() {
    FILE* file = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t input[kDecoderInputBytes];
    uint8_t* output = static_cast<uint8_t*>(malloc(kDecoderOutputBytes));
    int16_t* converted = static_cast<int16_t*>(heap_caps_malloc(
        kConvertedSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (converted == nullptr) {
        converted = static_cast<int16_t*>(malloc(kConvertedSamples * sizeof(int16_t)));
    }
    size_t output_capacity = kDecoderOutputBytes;
    uint32_t current_token = 0;
    uint32_t sample_rate = 0;
    uint32_t phase = 0;
    int16_t previous = 0;
    bool have_previous = false;
    bool loop = false;
    bool stack_reported = false;
    if (output == nullptr || converted == nullptr) {
        free(output);
        free(converted);
        ESP_LOGE(kTag, "Cannot allocate Ogg decoder buffers");
        music_active_.store(false);
        music_voice_.store(-1);
        return;
    }
    for (;;) {
        const uint32_t requested_token = music_token_.load();
        if (current_token != requested_token) {
            if (decoder != nullptr) esp_audio_simple_dec_close(decoder);
            if (file != nullptr) fclose(file);
            decoder = nullptr;
            file = nullptr;
            current_token = requested_token;
            if (xSemaphoreTake(music_mutex_, portMAX_DELAY) == pdTRUE) {
                music_read_ = music_count_ = 0;
                xSemaphoreGive(music_mutex_);
            }
        }

        MusicCommand command = {};
        if (xQueueReceive(music_commands_, &command, 0) == pdTRUE &&
            command.token == current_token) {
            file = fopen(command.path, "rb");
            esp_audio_simple_dec_cfg_t configuration = {};
            configuration.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
            if (file == nullptr ||
                esp_audio_simple_dec_open(&configuration, &decoder) !=
                    ESP_AUDIO_ERR_OK) {
                ESP_LOGW(kTag, "Cannot decode package music: %s", command.path);
                if (file != nullptr) fclose(file);
                file = nullptr;
                music_active_.store(false);
                music_voice_.store(-1);
                continue;
            }
            loop = command.loop;
            sample_rate = phase = 0;
            have_previous = false;
            ESP_LOGI(kTag, "Playing Ogg asset: %s", command.path);
        }
        if (file == nullptr || decoder == nullptr || music_paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        const size_t room = kMusicSamples - music_count_;
        xSemaphoreGive(music_mutex_);
        if (room < 2048) {
            vTaskDelay(1);
            continue;
        }

        const size_t input_size = fread(input, 1, sizeof(input), file);
        if (input_size == 0) {
            if (loop && !ferror(file) && fseek(file, 0, SEEK_SET) == 0 &&
                esp_audio_simple_dec_reset(decoder) == ESP_AUDIO_ERR_OK) {
                sample_rate = phase = 0;
                have_previous = false;
                continue;
            }
            music_active_.store(false);
            music_voice_.store(-1);
            music_token_.fetch_add(1);
            continue;
        }
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input;
        raw.len = input_size;
        raw.eos = input_size < sizeof(input);
        bool failed = false;
        while (raw.len != 0 && music_token_.load() == current_token) {
            esp_audio_simple_dec_out_t decoded = {};
            decoded.buffer = output;
            decoded.len = output_capacity;
            const esp_audio_err_t error = esp_audio_simple_dec_process(
                decoder, &raw, &decoded);
            if (error == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                decoded.needed_size <= 65536) {
                auto* grown = static_cast<uint8_t*>(realloc(output,
                                                             decoded.needed_size));
                if (grown != nullptr) {
                    output = grown;
                    output_capacity = decoded.needed_size;
                    continue;
                }
            }
            if (error != ESP_AUDIO_ERR_OK || raw.consumed > raw.len ||
                (raw.consumed == 0 && decoded.decoded_size == 0)) {
                failed = true;
                break;
            }
            if (decoded.decoded_size != 0) {
                if (!stack_reported) {
                    ESP_LOGI(kTag, "Music decoder stack free: %u bytes",
                             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
                    stack_reported = true;
                }
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) !=
                        ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
                    info.sample_rate < 16000 || info.channel < 1 ||
                    info.channel > 2) {
                    failed = true;
                    break;
                }
                if (sample_rate != info.sample_rate) {
                    sample_rate = info.sample_rate;
                    phase = 0;
                    have_previous = false;
                }
                const auto* pcm = reinterpret_cast<const int16_t*>(decoded.buffer);
                const size_t frames = decoded.decoded_size /
                    (info.channel * sizeof(int16_t));
                size_t converted_count = 0;
                for (size_t index = 0; index < frames; ++index) {
                    const int32_t mono = info.channel == 2 ?
                        (static_cast<int32_t>(pcm[index * 2]) +
                         pcm[index * 2 + 1]) / 2 : pcm[index];
                    if (!have_previous) {
                        previous = static_cast<int16_t>(mono);
                        have_previous = true;
                        continue;
                    }
                    phase += 16000;
                    while (phase >= sample_rate) {
                        phase -= sample_rate;
                        converted[converted_count++] = static_cast<int16_t>(
                            previous + (mono - previous) *
                            static_cast<int32_t>(16000 - phase) / 16000);
                        if (converted_count == kConvertedSamples) {
                            if (!QueueMusic(converted, converted_count,
                                            current_token)) failed = true;
                            converted_count = 0;
                        }
                    }
                    previous = static_cast<int16_t>(mono);
                    if (failed) break;
                }
                if (converted_count != 0 &&
                    !QueueMusic(converted, converted_count, current_token))
                    failed = true;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (failed) break;
        }
        if (failed && music_token_.load() == current_token) {
            ESP_LOGW(kTag, "Ogg decoding stopped");
            music_active_.store(false);
            music_voice_.store(-1);
            music_token_.fetch_add(1);
        }
    }
}

void Rpc701Audio::Run() {
    OutputFrame frame;
    struct PlayingSound {
        const uint8_t* pcm;
        uint32_t samples;
        uint32_t position;
        int32_t gain_q15;
    };
    PlayingSound sounds[kSoundVoices] = {};
    TickType_t last_send = xTaskGetTickCount();
    int32_t output_music_gain_q15 = 0;
    int32_t source_music_gain_q15 = 0;
    int16_t last_music_sample = 0;
    for (;;) {
        uint8_t requested_volume;
        if (xQueueReceive(volume_queue_, &requested_volume, 0) == pdTRUE) {
            if (rpc_adapter_is_slave_ready()) {
                const int result = rpc_music_volume_set(requested_volume);
                if (result != RPC_ERR_SUCCESS) {
                    ESP_LOGW(kTag, "Volume %u rejected: %d",
                             static_cast<unsigned>(requested_volume), result);
                }
            } else {
                xQueueOverwrite(volume_queue_, &requested_volume);
            }
        }

        SoundEvent sound_event;
        while (sound_events_ != nullptr &&
               xQueueReceive(sound_events_, &sound_event, 0) == pdTRUE) {
            for (auto& sound : sounds) {
                if (sound.position < sound.samples) continue;
                sound = {sound_event.pcm, sound_event.samples, 0,
                         sound_event.gain_q15};
                break;
            }
        }
        bool sounds_active = false;
        for (const auto& sound : sounds)
            sounds_active |= sound.position < sound.samples;
        const bool music_active = music_active_.load();
        if (xQueueReceive(output_queue_, &frame,
                          music_active || sounds_active ||
                              source_music_gain_q15 != 0
                              ? 0 : pdMS_TO_TICKS(10)) != pdTRUE) {
            if (!music_active && !sounds_active &&
                source_music_gain_q15 == 0 &&
                (sound_events_ == nullptr ||
                 uxQueueMessagesWaiting(sound_events_) == 0)) {
                last_send = xTaskGetTickCount();
                continue;
            }
            frame.samples = kFrameSamples;
            memset(frame.pcm, 0, sizeof(frame.pcm));
        }
        {
            const bool music_playing = music_active && !music_paused_.load();
            int16_t music[kFrameSamples] = {};
            size_t available = 0;
            if (music_playing &&
                xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
                available = std::min(static_cast<size_t>(frame.samples),
                                     music_count_);
                for (size_t index = 0; index < available; ++index) {
                    music[index] = music_ring_[music_read_];
                    music_read_ = (music_read_ + 1) % kMusicSamples;
                }
                music_count_ -= available;
                xSemaphoreGive(music_mutex_);
            }
            const int32_t target_gain = music_playing
                ? music_gain_q15_.load() : 0;
            for (size_t index = 0; index < frame.samples; ++index) {
                if (output_music_gain_q15 < target_gain) {
                    output_music_gain_q15 = std::min(
                        output_music_gain_q15 + 256, target_gain);
                } else if (output_music_gain_q15 > target_gain) {
                    output_music_gain_q15 = std::max(
                        output_music_gain_q15 - 256, target_gain);
                }
                if (index < available) last_music_sample = music[index];
                const int32_t source_target = index < available ? 32768 : 0;
                if (source_music_gain_q15 < source_target) {
                    source_music_gain_q15 = std::min(
                        source_music_gain_q15 + 512, source_target);
                } else if (source_music_gain_q15 > source_target) {
                    source_music_gain_q15 = std::max(
                        source_music_gain_q15 - 512, source_target);
                }
                const int32_t music_sample =
                    (static_cast<int32_t>(last_music_sample) *
                     source_music_gain_q15) >> 15;
                int32_t mixed = frame.pcm[index] +
                    ((music_sample * output_music_gain_q15) >> 15);
                for (auto& sound : sounds) {
                    if (sound.position >= sound.samples) continue;
                    const uint32_t remaining = sound.samples - sound.position;
                    const uint32_t attack = sound.position + 1;
                    const uint32_t envelope = std::min<uint32_t>(
                        64, std::min(attack, remaining));
                    const int32_t sample =
                        (static_cast<int32_t>(sound.pcm[sound.position++]) - 128)
                        * 256;
                    mixed += ((sample * sound.gain_q15) >> 15) *
                             static_cast<int32_t>(envelope) / 64;
                }
                frame.pcm[index] = static_cast<int16_t>(std::clamp(
                    mixed, static_cast<int32_t>(INT16_MIN),
                    static_cast<int32_t>(INT16_MAX)));
            }
        }
        for (size_t offset = 0; offset < frame.samples; offset += kRpcPcmSamples) {
            const size_t chunk = std::min(kRpcPcmSamples,
                                          static_cast<size_t>(frame.samples) - offset);
            if (rpc_adapter_is_slave_ready()) {
                const int result = rpc_sen_audio_pcm(
                    reinterpret_cast<uint8_t*>(frame.pcm + offset),
                    chunk * sizeof(int16_t));
                if (result != RPC_ERR_SUCCESS) {
                    ESP_LOGW(kTag, "PCM transfer failed: %d", result);
                }
            }
            vTaskDelayUntil(&last_send, pdMS_TO_TICKS(10));
        }
    }
}
