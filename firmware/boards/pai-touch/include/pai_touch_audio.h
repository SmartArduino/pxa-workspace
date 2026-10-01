#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <pxa/pxa_audio_output.h>

/* pai-touch speaker path. The board has no codec of its own: PCM and volume
 * leave through the JL701 coprocessor UART link in `jl701_audio_link.h`, and
 * the RPC701 vendor stack stays private to the board component. */
class PaiTouchAudio {
public:
    bool Initialize();
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return volume_.load(); }

private:
    static bool Write(void* context, const int16_t* pcm, size_t samples);
    PxaAudioOutput output_;
    std::atomic<uint8_t> volume_{70};
};
