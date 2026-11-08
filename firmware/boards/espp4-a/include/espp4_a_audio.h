// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <pxa/pxa_audio_output.h>
#include <pxa/pxa_audio_capture_input.h>

class Espp4Audio {
public:
    bool Initialize();
    bool SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }
private:
    bool Write(const int16_t*, size_t);
    bool CaptureEnabled(bool);
    int32_t Read(int16_t*, size_t);
    i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
    esp_codec_dev_handle_t codec_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
    std::atomic<uint8_t> volume_{60};
    int16_t stereo_[640]{};
    int16_t input_[640]{};
    PxaAudioOutput output_;
    PxaAudioCaptureInput capture_;
};
