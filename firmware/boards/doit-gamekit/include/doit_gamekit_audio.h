#pragma once
#include <atomic>
#include <driver/i2s_std.h>
#include <pxa/pxa_audio_output.h>

class DoitGamekitAudio {
public:
    bool Initialize();
    void SetVolume(uint8_t percent) { volume_.store(percent > 100 ? 100 : percent); }
    uint8_t volume() const { return volume_.load(); }
private:
    bool Write(const int16_t* pcm, size_t count);
    PxaAudioOutput output_;
    i2s_chan_handle_t channel_ = nullptr;
    std::atomic<uint8_t> volume_{65};
    // Only the single PxaAudioOutput worker writes this staging buffer.
    int16_t stereo_[320 * 2]{};
};
