#include "pai_touch_audio.h"

#include <algorithm>
#include <esp_log.h>

#include "jl701_audio_link.h"
#include "pai_touch_config.h"

namespace {
constexpr char kTag[] = "PaiTouchAudio";
}

bool PaiTouchAudio::Initialize() {
    const jl701_audio_link_config_t config = {
        .uart_num = PAI_JL701_UART,
        .tx_pin = PAI_JL701_TX,
        .rx_pin = PAI_JL701_RX,
        .baud_rate = PAI_JL701_BAUD,
        /* Remote amplifier control stays disabled: the current JL701 firmware
         * stops answering the handshake when it is asked to drive its own PA
         * pin, which is what the vendor reference firmware does too. */
        .pa_io = -1,
        .pa_en_level = 0,
    };
    if (!jl701_audio_link_start(&config)) {
        ESP_LOGE(kTag, "JL701 audio link initialization failed");
        return false;
    }
    if (!jl701_audio_link_ready()) {
        /* Expected on a cold boot: the coprocessor needs a few seconds after
         * the wake pulse before it answers, and the link repeats the handshake
         * in the background. PCM is dropped until it succeeds. */
        ESP_LOGI(kTag, "JL701 still booting after its wake pulse");
    }

    /* The Host writes mono 16 kHz frames into this sink; the link paces them
     * to the coprocessor. */
    return output_.Initialize(Write, this);
}

void PaiTouchAudio::SetVolume(uint8_t percent) {
    volume_.store(std::min<uint8_t>(percent, 100));
    jl701_audio_link_set_volume(volume_.load());
}

bool PaiTouchAudio::Write(void* context, const int16_t* pcm, size_t samples) {
    (void)context;
    return jl701_audio_link_write_pcm(pcm, samples);
}
