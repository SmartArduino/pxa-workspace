#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <pxa/pxa_audio_output.h>
#include "esp_mosaico_audio_transfer.h"

class EspMosaicoAudio {
public:
    bool Initialize(i2c_master_bus_handle_t bus);
    bool SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }
    void PublishDiagnostics();

private:
    static constexpr size_t kMaxWriteSamples = 1024;
    bool Write(const int16_t* pcm, size_t samples);
    bool Check(int result, const char* stage);
    bool RestartOutput();
    static bool OnDmaSent(i2s_chan_handle_t, i2s_event_data_t*, void* context);

    PxaAudioOutput output_;
    i2s_chan_handle_t tx_channel_ = nullptr;
    esp_codec_dev_handle_t speaker_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    SemaphoreHandle_t control_mutex_ = nullptr;
    int16_t stereo_[kMaxWriteSamples * 2] = {};
    std::atomic<uint8_t> volume_{70};
    const char* init_stage_ = "not_started";
    int init_error_ = 0;
    bool ready_ = false;
    std::atomic<uint32_t> writes_{0};
    std::atomic<uint32_t> nonzero_frames_{0};
    std::atomic<uint32_t> write_errors_{0};
    std::atomic<uint32_t> peak_{0};
    std::atomic<uint32_t> dma_blocks_{0};
    std::atomic<uint32_t> dma_nonzero_blocks_{0};
    std::atomic<uint32_t> recoveries_{0};
    mosaico_board::AudioDmaProgress dma_progress_;
};
