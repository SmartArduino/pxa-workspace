#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class Rpc701Audio {
public:
    bool Initialize();
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }
    size_t ReadOpus(uint8_t* output, size_t capacity);

private:
    static constexpr size_t kFrameSamples = 320;
    static constexpr size_t kMusicSamples = 8192;
    static constexpr size_t kMusicPathSize = 512;
    static constexpr size_t kSoundCacheEntries = 24;
    static constexpr size_t kSoundVoices = 6;
    struct OutputFrame {
        uint16_t samples;
        int16_t pcm[kFrameSamples];
    };
    struct MusicCommand {
        uint32_t token;
        bool loop;
        char path[kMusicPathSize];
    };
    struct SoundAsset {
        char path[kMusicPathSize];
        uint8_t* pcm;
        uint32_t samples;
    };
    struct SoundEvent {
        const uint8_t* pcm;
        uint32_t samples;
        int32_t gain_q15;
    };

    static bool Submit(void* context, uint8_t voice, const int16_t* pcm,
                       size_t samples);
    static void Flush(void* context, uint8_t voice);
    static bool PlayAsset(void* context, uint8_t voice,
                          const char* absolute_path, bool loop,
                          int16_t gain_db_q8);
    static bool ControlAsset(void* context, uint8_t voice,
                             uint8_t action, int16_t gain_db_q8);
    static void AudioTask(void* context);
    static void MusicTask(void* context);
    void Run();
    void RunMusic();
    bool QueueMusic(const int16_t* samples, size_t count, uint32_t token);

    QueueHandle_t output_queue_ = nullptr;
    QueueHandle_t volume_queue_ = nullptr;
    QueueHandle_t music_commands_ = nullptr;
    QueueHandle_t sound_events_ = nullptr;
    SemaphoreHandle_t music_mutex_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t music_task_ = nullptr;
    int16_t* music_ring_ = nullptr;
    SoundAsset* sound_cache_ = nullptr;
    size_t sound_cache_count_ = 0;
    size_t music_read_ = 0;
    size_t music_count_ = 0;
    std::atomic<int> music_voice_{-1};
    std::atomic<uint32_t> music_token_{0};
    std::atomic<int32_t> music_gain_q15_{0};
    std::atomic<bool> music_active_{false};
    std::atomic<bool> music_paused_{false};
    std::atomic<uint8_t> volume_{70};
};
