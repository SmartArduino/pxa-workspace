#pragma once
#include <atomic>
#include <pxa/pxa_audio_output.h>
class Rpc701Audio {
public:
    bool Initialize();
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }
    size_t ReadOpus(uint8_t* output, size_t capacity);
private:
    static bool Write(void* context, const int16_t* pcm, size_t samples);
    PxaAudioOutput output_;
    std::atomic<uint8_t> volume_{70};
    std::atomic<bool> volume_dirty_{true};
};
