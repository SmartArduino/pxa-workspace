#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

class Rpc701Audio {
public:
    bool Initialize();
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }
    size_t ReadOpus(uint8_t* output, size_t capacity);

private:
    static constexpr size_t kFrameSamples = 320;
    struct OutputFrame {
        uint16_t samples;
        int16_t pcm[kFrameSamples];
    };

    static bool Submit(void* context, uint8_t voice, const int16_t* pcm,
                       size_t samples);
    static void Flush(void* context, uint8_t voice);
    static void AudioTask(void* context);
    void Run();

    QueueHandle_t output_queue_ = nullptr;
    QueueHandle_t volume_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<uint8_t> volume_{70};
};

