#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Esp32S31Korvo1Audio {
public:
    bool Initialize(i2c_master_bus_handle_t i2c_bus);
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }

private:
    static bool Submit(void* context, uint8_t voice, const int16_t* pcm,
                       size_t samples);
    static void Flush(void* context, uint8_t voice);
    bool Write(const int16_t* pcm, size_t samples);

    i2s_chan_handle_t tx_channel_ = nullptr;
    i2s_chan_handle_t rx_channel_ = nullptr;
    const audio_codec_data_if_t* data_if_ = nullptr;
    esp_codec_dev_handle_t speaker_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::atomic<uint8_t> volume_{70};
};
