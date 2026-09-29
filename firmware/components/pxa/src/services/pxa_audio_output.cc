#include "pxa_esp_assets.h"
#include "pxa/pxa_audio_output.h"
#include "pxa_esp_resource_memory.h"
#include "pxa_codec_memory.h"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_dec_reg.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <pxa/pxa_host.h>
#include <pxa/audio.h>

namespace {
constexpr char kTag[] = "PxaAudioOutput";
constexpr size_t kDecoderInputBytes = 768;
constexpr size_t kDecoderOutputBytes = 16384;
constexpr size_t kConvertedSamples = 2048;
constexpr uint32_t kMusicTaskStackBytes = 20480;
constexpr uint32_t kOutputTaskStackBytes = 4096;
}

// ESP-IDF task stack depths are bytes. All RTOS backing storage is charged
// before creating any task; static creation cannot fail from heap pressure.
// This block and both tasks have the board's lifetime, not an app's lifetime.
struct PxaAudioOutput::RuntimeStorage {
    StaticQueue_t output_queue;
    StaticQueue_t music_commands;
    StaticSemaphore_t music_mutex;
    StaticTask_t output_task;
    StaticTask_t music_task;
    uint8_t output_frames[3 * sizeof(OutputFrame)];
    uint8_t music_command[sizeof(MusicCommand)];
    StackType_t output_stack[kOutputTaskStackBytes / sizeof(StackType_t)];
};
static_assert(kMusicTaskStackBytes % sizeof(StackType_t) == 0, "whole stack elements required");
static_assert(kOutputTaskStackBytes % sizeof(StackType_t) == 0, "whole stack elements required");

bool PxaAudioOutput::Initialize(Write write, void* context) {
    if (task_ != nullptr) return true;
    if (!write) return false;
    if (pxa_esp_resource_memory_initialize() != PXA_STATUS_OK) return false;
    PxaCodecMemoryScope codec_scope(pxa_esp_device_resource_allocator(
        PXA_MEMORY_EXTERNAL, PXA_MEMORY_METADATA));
    write_ = write;
    write_context_ = context;
    runtime_storage_ = static_cast<RuntimeStorage*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_METADATA),
        sizeof(RuntimeStorage)));
    // Keep the larger decoder stack separate: budgeting must not raise the
    // largest contiguous internal-RAM request from 20 KiB to almost 28 KiB.
    music_stack_ = static_cast<StackType_t*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_METADATA),
        kMusicTaskStackBytes));
    music_ring_ = static_cast<int16_t*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_AUDIO),
        kMusicSamples * sizeof(int16_t)));
    if (runtime_storage_) {
        output_queue_ = xQueueCreateStatic(3, sizeof(OutputFrame),
            runtime_storage_->output_frames, &runtime_storage_->output_queue);
        music_commands_ = xQueueCreateStatic(1, sizeof(MusicCommand),
            runtime_storage_->music_command, &runtime_storage_->music_commands);
        music_mutex_ = xSemaphoreCreateMutexStatic(&runtime_storage_->music_mutex);
    }
    bool vorbis_registered = false, opus_registered = false;
    if (!runtime_storage_ || !music_stack_ || !output_queue_ || !music_commands_ || !music_mutex_ ||
        !music_ring_ ||
        !(vorbis_registered = esp_vorbis_dec_register() == ESP_AUDIO_ERR_OK) ||
        !(opus_registered = esp_opus_dec_register() == ESP_AUDIO_ERR_OK) ||
        esp_ogg_dec_register() != ESP_AUDIO_ERR_OK) {
        if (opus_registered) esp_audio_dec_unregister(ESP_AUDIO_TYPE_OPUS);
        if (vorbis_registered) esp_audio_dec_unregister(ESP_AUDIO_TYPE_VORBIS);
        if (output_queue_) vQueueDelete(output_queue_);
        if (music_commands_) vQueueDelete(music_commands_);
        if (music_mutex_) vSemaphoreDelete(music_mutex_);
        pxa_memory_release(music_ring_);
        pxa_memory_release(runtime_storage_); runtime_storage_ = nullptr;
        pxa_memory_release(music_stack_); music_stack_ = nullptr;
        output_queue_ = music_commands_ = nullptr;
        music_mutex_ = nullptr; music_ring_ = nullptr;
        write_ = nullptr; write_context_ = nullptr;
        return false;
    }
    task_ = xTaskCreateStatic(AudioTask, "pxa_output", kOutputTaskStackBytes, this, 6,
        runtime_storage_->output_stack, &runtime_storage_->output_task);
    music_task_ = xTaskCreateStatic(MusicTask, "pxa_decode", kMusicTaskStackBytes, this, 5,
        music_stack_, &runtime_storage_->music_task);
    configASSERT(task_ && music_task_);
    ESP_LOGI(kTag, "Audio device storage: RTOS=%u (stacks=%u), ring=%u bytes",
        static_cast<unsigned>(sizeof(RuntimeStorage) + kMusicTaskStackBytes),
        static_cast<unsigned>(kOutputTaskStackBytes + kMusicTaskStackBytes),
        static_cast<unsigned>(kMusicSamples * sizeof(int16_t)));
    pxa_host_set_audio_sink(Submit, Flush, this);
    pxa_host_set_audio_asset_sink(PlayAsset, ControlAsset, this);
    pxa_host_set_audio_sound_sink(PlaySound,this);
    const pxa_host_audio_music_sink_t music_sink={this,PlayMusic,PlaybackPeek,PlaybackConsume,PlaybackClose};
    pxa_host_set_audio_music_sink(&music_sink);
    return true;
}

void PxaAudioOutput::WakeOutput() {
    const OutputFrame wake = {};
    // A full queue already guarantees that the output worker will wake.
    (void)xQueueSend(output_queue_, &wake, 0);
}

void PxaAudioOutput::ReleaseSound(PlayingSound& sound) {
    pxa_asset_object_release_pinned(sound.asset);
    sound = {};
}

bool PxaAudioOutput::Submit(void* context, uint8_t voice, const int16_t* pcm,
                         size_t samples) {
    auto* self = static_cast<PxaAudioOutput*>(context);
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

void PxaAudioOutput::Flush(void* context, uint8_t voice) {
    auto* self = static_cast<PxaAudioOutput*>(context);
    (void)voice;
    if (self != nullptr && self->output_queue_ != nullptr) {
        xQueueReset(self->output_queue_);
    }
}

bool PxaAudioOutput::PlaySound(void* context,uint8_t voice,pxa_asset_object_t* asset,int16_t gain) {
    auto* self = static_cast<PxaAudioOutput*>(context);
    if (!self || !self->music_mutex_ || voice >= 3 || gain > 0 || gain < -60*256) return false;
    pxa_asset_object_view_t view;
    pxa_asset_object_view(asset,&view);
    if (view.kind != PXA_ASSET_AUDIO || !view.bytes || view.bytes > 16000) return false;
    const int32_t gain_q15 = static_cast<int32_t>(32768.f * powf(10.f,gain/5120.f));
    bool accepted = false;
    xSemaphoreTake(self->music_mutex_,portMAX_DELAY);
    for (auto& sound : self->sounds_) if (!sound.asset) {
        pxa_asset_object_retain(asset);
        sound = {asset,view.data,view.bytes,0,gain_q15,voice};
        self->sound_paused_[voice] = false; accepted = true; break;
    }
    xSemaphoreGive(self->music_mutex_);
    if (accepted) self->WakeOutput();
    return accepted;
}

bool PxaAudioOutput::PlayAsset(void* context, uint8_t voice,
                             const char* absolute_path, bool loop,
                             int16_t gain_db_q8) {
    uint64_t ignored;
    return PlayMusic(context,voice,0,absolute_path,loop,gain_db_q8,false,&ignored)==PXA_STATUS_OK;
}

pxa_status_t PxaAudioOutput::PlayMusic(void* context,uint8_t voice,uint64_t session,
    const char* absolute_path,bool loop,int16_t gain_db_q8,bool paused,uint64_t* instance) {
    auto* self = static_cast<PxaAudioOutput*>(context);
    if (self == nullptr || self->music_commands_ == nullptr ||
        absolute_path == nullptr || !instance || voice>=3 || gain_db_q8>0 || gain_db_q8 < -60*256)
        return PXA_STATUS_INVALID_ARGUMENT;
    *instance=0;
    // Reserve the input state before admission. Its allocation pins
    // the captured budget owner; its catalog lease also survives app shutdown.
    MusicCommand command = {}, retired_command = {};
    command.temporary_allocator = pxa_esp_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY);
    pxa_status_t acquired = pxa_esp_music_input_acquire(absolute_path,
        command.temporary_allocator, &command.input);
    if (acquired) return acquired;
    xSemaphoreTake(self->music_mutex_, portMAX_DELAY);
    const int owner = self->music_voice_.load();
    if (owner >= 0 && owner != voice) {
        xSemaphoreGive(self->music_mutex_);
        pxa_esp_music_input_release(command.input);
        return PXA_STATUS_WOULD_BLOCK;
    }
    pxa_status_t admission=self->next_music_token_==UINT32_MAX ? PXA_STATUS_RESOURCE_LIMIT : PXA_STATUS_OK;
    if (admission==PXA_STATUS_OK && session)
        admission=pxa_audio_playback_begin(&self->music_events_,session,instance);
    if (admission!=PXA_STATUS_OK) {
        xSemaphoreGive(self->music_mutex_); pxa_esp_music_input_release(command.input); return admission;
    }
    pxa_audio_playback_finish(&self->music_events_,self->music_instance_,PXA_AUDIO_PLAYBACK_REPLACED,PXA_STATUS_OK);
    self->music_instance_=*instance;
    self->music_read_ = self->music_count_ = 0;
    self->music_eof_ = false;
    self->music_accepted_us_ = esp_timer_get_time();
    pxa_audio_buffer_start(&self->music_stats_.buffer,4096);
    command.loop = loop;
    command.token = ++self->next_music_token_;
    self->music_token_.store(command.token);
    self->music_voice_.store(voice);
    self->music_gain_q15_.store(static_cast<int32_t>(32768.0f *
        powf(10.0f, gain_db_q8 / (20.0f * 256.0f))));
    self->music_paused_.store(paused);
    self->music_active_.store(true);
    // There is one Host producer. Dequeue an overwritten command explicitly so
    // its activation pin cannot disappear with the queue's copied bytes.
    (void)xQueueReceive(self->music_commands_, &retired_command, 0);
    // A one-element overwrite cannot fail from queue pressure. All fallible
    // admission checks precede replacement, so synchronous rejection preserves
    // the previous playback. The mutex serializes producers and STOP.
    (void)xQueueOverwrite(self->music_commands_, &command);
    xSemaphoreGive(self->music_mutex_);
    pxa_esp_music_input_release(retired_command.input);
    self->WakeOutput();
    xTaskNotifyGive(self->music_task_);
    pxa_host_audio_notify();
    return PXA_STATUS_OK;
}

pxa_status_t PxaAudioOutput::PlaybackPeek(void* c,pxa_audio_playback_event_t* event) {
    auto* self=static_cast<PxaAudioOutput*>(c);
    xSemaphoreTake(self->music_mutex_,portMAX_DELAY);
    pxa_status_t status=pxa_audio_playback_peek(&self->music_events_,event);
    xSemaphoreGive(self->music_mutex_); return status;
}
pxa_status_t PxaAudioOutput::PlaybackConsume(void* c,const pxa_audio_playback_event_t* event) {
    auto* self=static_cast<PxaAudioOutput*>(c);
    xSemaphoreTake(self->music_mutex_,portMAX_DELAY);
    pxa_status_t status=pxa_audio_playback_consume(&self->music_events_,event);
    xSemaphoreGive(self->music_mutex_); return status;
}
void PxaAudioOutput::PlaybackClose(void* c,uint64_t session) {
    auto* self=static_cast<PxaAudioOutput*>(c);
    xSemaphoreTake(self->music_mutex_,portMAX_DELAY);
    pxa_audio_playback_close(&self->music_events_,session);
    xSemaphoreGive(self->music_mutex_);
}

bool PxaAudioOutput::ControlAsset(void* context, uint8_t voice,
                                uint8_t action, int16_t gain_db_q8) {
    auto* self = static_cast<PxaAudioOutput*>(context);
    if (self == nullptr || self->music_task_ == nullptr || voice >= 3) return false;
    if (action < PXA_AUDIO_ASSET_PAUSE || action > PXA_AUDIO_ASSET_SET_GAIN) return false;
    MusicCommand retired_command = {};
    xSemaphoreTake(self->music_mutex_, portMAX_DELAY);
    if (action == PXA_AUDIO_ASSET_PAUSE) self->sound_paused_[voice] = true;
    if (action == PXA_AUDIO_ASSET_RESUME) self->sound_paused_[voice] = false;
    for (auto& sound : self->sounds_) {
        if (!sound.asset || sound.voice != voice) continue;
        if (action == PXA_AUDIO_ASSET_STOP) self->ReleaseSound(sound);
        else if (action == PXA_AUDIO_ASSET_SET_GAIN)
            sound.gain_q15 = static_cast<int32_t>(32768.f * powf(10.f, gain_db_q8 / 5120.f));
    }
    if (self->music_voice_.load() == voice) {
        switch (action) {
            case PXA_AUDIO_ASSET_PAUSE: self->music_paused_.store(true); break;
            case PXA_AUDIO_ASSET_RESUME: self->music_paused_.store(false); break;
            case PXA_AUDIO_ASSET_STOP:
                pxa_audio_playback_finish(&self->music_events_,self->music_instance_,PXA_AUDIO_PLAYBACK_STOPPED,PXA_STATUS_OK);
                self->music_instance_=0;
                self->music_active_.store(false);
                self->music_voice_.store(-1);
                self->music_token_.store(0);
                self->music_read_ = self->music_count_ = 0;
                (void)xQueueReceive(self->music_commands_, &retired_command, 0);
                break;
            case PXA_AUDIO_ASSET_SET_GAIN:
                self->music_gain_q15_.store(static_cast<int32_t>(32768.f *
                    powf(10.f, gain_db_q8 / 5120.f))); break;
        }
    }
    xSemaphoreGive(self->music_mutex_);
    pxa_esp_music_input_release(retired_command.input);
    self->WakeOutput();
    xTaskNotifyGive(self->music_task_);
    pxa_host_audio_notify();
    return true;
}

void PxaAudioOutput::FinishMusic(uint32_t token,pxa_status_t status) {
    xSemaphoreTake(music_mutex_, portMAX_DELAY);
    if (music_token_.load() == token) {
        pxa_audio_playback_finish(&music_events_,music_instance_,status ? PXA_AUDIO_PLAYBACK_ERROR : PXA_AUDIO_PLAYBACK_ENDED,status);
        music_instance_=0;
        music_active_.store(false);
        music_voice_.store(-1);
        music_token_.store(0);
    }
    xSemaphoreGive(music_mutex_);
    pxa_host_audio_notify();
}

void PxaAudioOutput::AudioTask(void* context) {
    static_cast<PxaAudioOutput*>(context)->Run();
    vTaskDelete(nullptr);
}

void PxaAudioOutput::MusicTask(void* context) {
    static_cast<PxaAudioOutput*>(context)->RunMusic();
    vTaskDelete(nullptr);
}

bool PxaAudioOutput::GetMusicStats(MusicStats* output) {
    if (!output || !music_mutex_) return false;
    xSemaphoreTake(music_mutex_,portMAX_DELAY);
    *output=music_stats_;
    xSemaphoreGive(music_mutex_);
    return true;
}

bool PxaAudioOutput::PublishMusicLocked() {
    if (!pxa_audio_buffer_publish(&music_stats_.buffer,music_count_,music_eof_)) return false;
    pxa_audio_playback_ready(&music_events_,music_instance_);
    const uint64_t elapsed=static_cast<uint64_t>(esp_timer_get_time())-music_accepted_us_;
    music_stats_.ready_max_us=std::max(music_stats_.ready_max_us,elapsed);
    return true;
}

bool PxaAudioOutput::QueueMusic(const int16_t* samples, size_t count,
                              uint32_t token) {
    size_t copied = 0;
    while (copied < count && music_token_.load() == token) {
        if (xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        if (music_token_.load() != token) { xSemaphoreGive(music_mutex_); break; }
        if (music_paused_.load()) {
            xSemaphoreGive(music_mutex_);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const size_t available = std::min(count - copied,
                                           kMusicSamples - music_count_);
        for (size_t index = 0; index < available; ++index) {
            const size_t position = (music_read_ + music_count_) % kMusicSamples;
            music_ring_[position] = samples[copied + index];
            ++music_count_;
        }
        const bool ready=PublishMusicLocked();
        xSemaphoreGive(music_mutex_);
        if (ready) pxa_host_audio_notify();
        copied += available;
        if (available == 0) vTaskDelay(1);
    }
    return copied == count;
}

void PxaAudioOutput::RunMusic() {
    pxa_esp_music_input_t* file = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t input[kDecoderInputBytes];
    uint8_t* output = nullptr;
    int16_t* converted = nullptr;
    size_t output_capacity = 0;
    uint32_t current_token = 0;
    const pxa_memory_allocator_t* temporary_allocator = nullptr;
    struct Cancellation { PxaAudioOutput* self; uint32_t token; } cancellation = {this, 0};
    uint32_t sample_rate = 0;
    uint32_t phase = 0;
    int16_t previous = 0;
    bool have_previous = false;
    bool loop = false;
    bool stack_reported = false;
    for (;;) {
        PxaCodecMemoryScope codec_scope(temporary_allocator);
        const uint32_t requested_token = music_token_.load();
        if (current_token != requested_token) {
            if (decoder != nullptr) esp_audio_simple_dec_close(decoder);
            pxa_esp_music_input_release(file);
            decoder = nullptr;
            file = nullptr;
            if (current_token) {
                MusicStats stats;
                if (GetMusicStats(&stats)) {
                    ESP_LOGI(kTag, "Music cumulative: underruns=%llu recovered=%llu missing=%llu consumed=%llu high_water=%u low_water=%u low_water_valid=%u ready_max_us=%llu read_calls=%llu read_bytes=%llu read_max_bytes=%u read_us=%llu read_max_us=%llu decode_calls=%llu decode_us=%llu decode_max_us=%llu",
                        (unsigned long long)stats.buffer.underruns,(unsigned long long)stats.buffer.recoveries,
                        (unsigned long long)stats.buffer.missing_samples,(unsigned long long)stats.buffer.consumed_samples,
                        (unsigned)stats.buffer.high_water,(unsigned)stats.buffer.low_water,(unsigned)stats.buffer.low_water_valid,
                        (unsigned long long)stats.ready_max_us,
                        (unsigned long long)stats.read_calls,(unsigned long long)stats.read_bytes,(unsigned)stats.read_max_bytes,
                        (unsigned long long)stats.read_us,(unsigned long long)stats.read_max_us,
                        (unsigned long long)stats.decode_calls,(unsigned long long)stats.decode_us,(unsigned long long)stats.decode_max_us);
                }
            }
            current_token = requested_token;
            // Buffers belong to the command's activation, not the device.
            // Retiring the token always releases them, including replacement.
            pxa_memory_release(output); pxa_memory_release(converted);
            output = nullptr; converted = nullptr; output_capacity = 0;
            temporary_allocator = nullptr;
            codec_scope.Set(nullptr);
        }

        MusicCommand command = {};
        if (xQueueReceive(music_commands_, &command, file ? 0 : portMAX_DELAY) == pdTRUE) {
            if (command.token != music_token_.load()) {
                pxa_esp_music_input_release(command.input);
                continue;
            }
            if (decoder != nullptr) esp_audio_simple_dec_close(decoder);
            pxa_esp_music_input_release(file);
            decoder = nullptr;
            pxa_memory_release(output); pxa_memory_release(converted);
            output = nullptr; converted = nullptr;
            file = command.input;
            cancellation.token = command.token;
            temporary_allocator = command.temporary_allocator;
            codec_scope.Set(temporary_allocator);
            current_token = command.token;
            if (!output) {
                output = static_cast<uint8_t*>(pxa_memory_allocate(temporary_allocator, kDecoderOutputBytes));
                output_capacity = kDecoderOutputBytes;
            }
            if (!converted) converted = static_cast<int16_t*>(pxa_memory_allocate(
                temporary_allocator, kConvertedSamples * sizeof(int16_t)));
            if (!output || !converted) {
                ESP_LOGW(kTag, "Cannot allocate music decode buffers");
                FinishMusic(current_token,PXA_STATUS_RESOURCE_LIMIT);
                continue;
            }
            pxa_status_t input_status = pxa_esp_music_input_open(file, &cancellation,
                [](void* context) -> int {
                    auto* state = static_cast<Cancellation*>(context);
                    return state->self->music_token_.load() != state->token;
                });
            esp_audio_simple_dec_cfg_t configuration = {};
            configuration.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
            const esp_audio_err_t open_error = input_status ? ESP_AUDIO_ERR_OK :
                esp_audio_simple_dec_open(&configuration, &decoder);
            if (input_status || open_error != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(kTag, "Cannot open package music decoder");
                FinishMusic(current_token, input_status ? input_status :
                    open_error == ESP_AUDIO_ERR_MEM_LACK ? PXA_STATUS_RESOURCE_LIMIT :
                    open_error == ESP_AUDIO_ERR_NOT_SUPPORT ? PXA_STATUS_UNSUPPORTED : PXA_STATUS_PROTOCOL_ERROR);
                continue;
            }
            loop = command.loop;
            sample_rate = phase = 0;
            have_previous = false;
            ESP_LOGI(kTag, "Playing Ogg asset");
        }
        if (file == nullptr || decoder == nullptr) continue;
        if (music_paused_.load()) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        const size_t room = kMusicSamples - music_count_;
        xSemaphoreGive(music_mutex_);
        if (room < 2048) {
            vTaskDelay(1);
            continue;
        }

        size_t input_size = 0;
        const uint64_t read_started=esp_timer_get_time();
        pxa_status_t read_status = pxa_esp_music_input_read(file, input, sizeof(input), &input_size);
        const uint64_t read_elapsed=static_cast<uint64_t>(esp_timer_get_time())-read_started;
        xSemaphoreTake(music_mutex_,portMAX_DELAY);
        ++music_stats_.read_calls; music_stats_.read_us+=read_elapsed;
        music_stats_.read_bytes+=input_size;
        music_stats_.read_max_bytes=std::max(music_stats_.read_max_bytes,static_cast<uint32_t>(input_size));
        music_stats_.read_max_us=std::max(music_stats_.read_max_us,read_elapsed);
        xSemaphoreGive(music_mutex_);
        if (read_status) { FinishMusic(current_token, read_status); continue; }
        if (input_size == 0) {
            if (loop && have_previous) {
                pxa_status_t rewind_status = pxa_esp_music_input_rewind(file);
                if (rewind_status || esp_audio_simple_dec_reset(decoder) != ESP_AUDIO_ERR_OK) {
                    FinishMusic(current_token, rewind_status ? rewind_status : PXA_STATUS_PROTOCOL_ERROR);
                    continue;
                }
                sample_rate = phase = 0;
                have_previous = false;
                continue;
            }
            // Release a short final track/tail even if it never reaches the
            // normal prebuffer threshold. EOF is published before draining.
            xSemaphoreTake(music_mutex_,portMAX_DELAY);
            bool ready=false;
            if (music_token_.load()==current_token) {
                music_eof_=true;
                ready=PublishMusicLocked();
            }
            xSemaphoreGive(music_mutex_);
            if (ready) pxa_host_audio_notify();
            bool drained = false;
            while (music_token_.load() == current_token && !drained) {
                xSemaphoreTake(music_mutex_, portMAX_DELAY);
                drained = music_count_ == 0 && music_output_token_ != current_token;
                xSemaphoreGive(music_mutex_);
                if (!drained) {
                    if (music_paused_.load()) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                    else vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            FinishMusic(current_token);
            continue;
        }
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = input;
        raw.len = input_size;
        // Short bounded reads do not necessarily mean EOF.
        raw.eos = pxa_esp_music_input_eof(file);
        bool failed = false;
        pxa_status_t decode_failure = PXA_STATUS_PROTOCOL_ERROR;
        while (raw.len != 0 && music_token_.load() == current_token) {
            esp_audio_simple_dec_out_t decoded = {};
            decoded.buffer = output;
            decoded.len = output_capacity;
            const uint64_t decode_started=esp_timer_get_time();
            const esp_audio_err_t error = esp_audio_simple_dec_process(
                decoder, &raw, &decoded);
            const uint64_t decode_elapsed=static_cast<uint64_t>(esp_timer_get_time())-decode_started;
            xSemaphoreTake(music_mutex_,portMAX_DELAY);
            ++music_stats_.decode_calls; music_stats_.decode_us+=decode_elapsed;
            music_stats_.decode_max_us=std::max(music_stats_.decode_max_us,decode_elapsed);
            xSemaphoreGive(music_mutex_);
            if (error == ESP_AUDIO_ERR_MEM_LACK) decode_failure = PXA_STATUS_RESOURCE_LIMIT;
            if (error == ESP_AUDIO_ERR_NOT_SUPPORT) decode_failure = PXA_STATUS_UNSUPPORTED;
            if (error == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
                decoded.needed_size > output_capacity && decoded.needed_size <= 65536) {
                auto* grown = static_cast<uint8_t*>(pxa_memory_resize(
                    temporary_allocator, output, decoded.needed_size));
                if (grown != nullptr) {
                    output = grown;
                    output_capacity = decoded.needed_size;
                    continue;
                }
                decode_failure = PXA_STATUS_RESOURCE_LIMIT;
            }
            if (error != ESP_AUDIO_ERR_OK || raw.consumed > raw.len ||
                decoded.decoded_size > output_capacity ||
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
                    info.sample_rate < 8000 || info.sample_rate > 96000 || info.channel < 1 ||
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
            FinishMusic(current_token,decode_failure);
        }
    }
}

void PxaAudioOutput::Run() {
    OutputFrame frame;
    TickType_t last_send = xTaskGetTickCount();
    int32_t output_music_gain_q15 = 0;
    int32_t source_music_gain_q15 = 0;
    int16_t last_music_sample = 0;
    uint32_t rendered_token = 0;
    for (;;) {
        uint32_t frame_music_token=0;
        bool sounds_active = false;
        xSemaphoreTake(music_mutex_, portMAX_DELAY);
        for (const auto& sound : sounds_)
            sounds_active |= sound.asset && !sound_paused_[sound.voice];
        xSemaphoreGive(music_mutex_);
        if (rendered_token != music_token_.load()) {
            rendered_token = music_token_.load();
            last_music_sample = 0;
            output_music_gain_q15 = source_music_gain_q15 = 0;
        }
        const bool music_active = music_active_.load();
        if (xQueueReceive(output_queue_, &frame,
                          (music_active && !music_paused_.load()) || sounds_active ||
                              source_music_gain_q15 != 0
                              ? 0 : portMAX_DELAY) != pdTRUE) {
            if ((!music_active || music_paused_.load()) && !sounds_active &&
                source_music_gain_q15 == 0) {
                last_send = xTaskGetTickCount();
                continue;
            }
            frame.samples = kFrameSamples;
            memset(frame.pcm, 0, sizeof(frame.pcm));
        }
        if (!frame.samples) continue;
        {
            const bool music_playing = music_active && !music_paused_.load();
            int16_t music[kFrameSamples] = {};
            size_t available = 0;
            if (music_playing &&
                xSemaphoreTake(music_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
                if (music_active_.load() && !music_paused_.load())
                    available=pxa_audio_buffer_take(&music_stats_.buffer,music_count_,frame.samples,music_eof_);
                for (size_t index = 0; index < available; ++index) {
                    music[index] = music_ring_[music_read_];
                    music_read_ = (music_read_ + 1) % kMusicSamples;
                }
                music_count_ -= available;
                if (available) frame_music_token=music_output_token_=music_token_.load();
                xSemaphoreGive(music_mutex_);
            }
            const int32_t target_gain = music_playing
                ? music_gain_q15_.load() : 0;
            xSemaphoreTake(music_mutex_, portMAX_DELAY);
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
                    static_cast<int32_t>((static_cast<int64_t>(music_sample) * output_music_gain_q15) >> 15);
                for (auto& sound : sounds_) {
                    if (!sound.asset || sound_paused_[sound.voice]) continue;
                    const uint32_t remaining = sound.samples - sound.position;
                    const uint32_t attack = sound.position + 1;
                    const uint32_t envelope = std::min<uint32_t>(
                        64, std::min(attack, remaining));
                    const int32_t sample =
                        (static_cast<int32_t>(sound.pcm[sound.position++]) - 128)
                        * 256;
                    mixed += static_cast<int32_t>((static_cast<int64_t>(sample) * sound.gain_q15) >> 15) *
                             static_cast<int32_t>(envelope) / 64;
                    if (sound.position == sound.samples) ReleaseSound(sound);
                }
                frame.pcm[index] = static_cast<int16_t>(std::clamp(
                    mixed, static_cast<int32_t>(INT16_MIN),
                    static_cast<int32_t>(INT16_MAX)));
            }
        }
        xSemaphoreGive(music_mutex_);
        const bool written=write_(write_context_, frame.pcm, frame.samples);
        if (!written)
            ESP_LOGW(kTag, "Physical audio write failed");
        // Keep the in-flight marker set until failure is latched, so the
        // decoder cannot win the race by publishing successful EOF first.
        if (!written && frame_music_token) FinishMusic(frame_music_token,PXA_STATUS_IO_ERROR);
        xSemaphoreTake(music_mutex_,portMAX_DELAY);
        if (music_output_token_==frame_music_token) music_output_token_=0;
        xSemaphoreGive(music_mutex_);
        vTaskDelayUntil(&last_send, pdMS_TO_TICKS(20));
        if ((TickType_t)(xTaskGetTickCount() - last_send) > pdMS_TO_TICKS(20))
            last_send = xTaskGetTickCount();
    }
}
