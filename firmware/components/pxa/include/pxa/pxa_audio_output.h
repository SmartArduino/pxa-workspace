#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include "pxa/resource_budget.h"
#include "pxa/asset_object.h"
#include "pxa/audio_playback.h"
#include "pxa/audio_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

struct pxa_esp_music_input;

class PxaAudioOutput {
public:
    using Write = bool (*)(void*, const int16_t*, size_t);
    // One board-owned instance, lifetime equals the board. Write may block;
    // Guest submit never calls the physical device or decoder directly.
    bool Initialize(Write write, void* context);
    struct MusicStats {
        pxa_audio_buffer_t buffer = {};
        uint64_t ready_max_us = 0;
        uint64_t read_calls = 0, read_bytes = 0, read_us = 0, read_max_us = 0;
        uint32_t read_max_bytes = 0;
        uint64_t decode_calls = 0, decode_us = 0, decode_max_us = 0;
    };
    // Device-cumulative counters. Short serialized snapshot; no I/O/allocation.
    bool GetMusicStats(MusicStats* output);


private:
    static constexpr size_t kFrameSamples = 320;
    static constexpr size_t kMusicSamples = 8192;
    static constexpr size_t kSoundVoices = 6;
    struct OutputFrame {
        uint16_t samples;
        int16_t pcm[kFrameSamples];
    };
    struct MusicCommand {
        uint32_t token;
        bool loop;
        const pxa_memory_allocator_t* temporary_allocator;
        pxa_esp_music_input* input; // Owns the catalog lease and bounded workspace.
    };
    struct PlayingSound {
        pxa_asset_object_t* asset = nullptr;
        const uint8_t* pcm = nullptr;
        uint32_t samples = 0;
        uint32_t position = 0;
        int32_t gain_q15 = 0;
        uint8_t voice = 0;
    };
    PlayingSound sounds_[kSoundVoices] = {};
    bool sound_paused_[3] = {};
    Write write_ = nullptr;
    void* write_context_ = nullptr;
    void ReleaseSound(PlayingSound& sound);

    static bool Submit(void* context, uint8_t voice, const int16_t* pcm,
                       size_t samples);
    static void Flush(void* context, uint8_t voice);
    static bool PlaySound(void*, uint8_t, pxa_asset_object_t*, int16_t);
    static bool PlayAsset(void* context, uint8_t voice,
                          const char* absolute_path, bool loop,
                          int16_t gain_db_q8);
    static pxa_status_t PlayMusic(void*,uint8_t,uint64_t,const char*,bool,int16_t,bool,uint64_t*);
    static pxa_status_t PlaybackPeek(void*,pxa_audio_playback_event_t*);
    static pxa_status_t PlaybackConsume(void*,const pxa_audio_playback_event_t*);
    static void PlaybackClose(void*,uint64_t);
    static bool ControlAsset(void* context, uint8_t voice,
                             uint8_t action, int16_t gain_db_q8);
    static void AudioTask(void* context);
    static void MusicTask(void* context);
    void Run();
    void WakeOutput();
    void RunMusic();
    void FinishMusic(uint32_t token,pxa_status_t status=PXA_STATUS_OK);
    bool QueueMusic(const int16_t* samples, size_t count, uint32_t token);
    bool PublishMusicLocked();

    QueueHandle_t output_queue_ = nullptr;
    struct RuntimeStorage;
    RuntimeStorage* runtime_storage_ = nullptr;
    StackType_t* music_stack_ = nullptr;
    QueueHandle_t music_commands_ = nullptr;
    SemaphoreHandle_t music_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t music_task_ = nullptr;
    int16_t* music_ring_ = nullptr;
    size_t music_read_ = 0;
    size_t music_count_ = 0;
    MusicStats music_stats_;
    uint64_t music_accepted_us_ = 0;
    bool music_eof_ = false;
    std::atomic<int> music_voice_{-1};
    std::atomic<uint32_t> music_token_{0};
    uint32_t next_music_token_ = 0;
    uint32_t music_output_token_ = 0;
    uint64_t music_instance_ = 0;
    pxa_audio_playback_queue_t music_events_ = {};
    std::atomic<int32_t> music_gain_q15_{0};
    std::atomic<bool> music_active_{false};
    std::atomic<bool> music_paused_{false};
};
