#include "doit_gamekit_audio.h"
#include "doit_gamekit_config.h"
#include <algorithm>
#include <esp_log.h>

bool DoitGamekitAudio::Initialize() {
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    config.dma_desc_num = 3;
    config.dma_frame_num = 320;
    config.auto_clear = true;
    if (i2s_new_channel(&config, &channel_, nullptr) != ESP_OK) return false;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(doit_gamekit::kAudioSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,
            .bclk = gpio_num_t(doit_gamekit::kAudioBclk),
            .ws = gpio_num_t(doit_gamekit::kAudioWs),
            .dout = gpio_num_t(doit_gamekit::kAudioDout),
            .din = GPIO_NUM_NC,
        },
    };
    if (i2s_channel_init_std_mode(channel_, &standard) != ESP_OK ||
        i2s_channel_enable(channel_) != ESP_OK) return false;
    if (!output_.Initialize([](void* self, const int16_t* pcm, size_t count) {
            return static_cast<DoitGamekitAudio*>(self)->Write(pcm, count);
        }, this, true, doit_gamekit::kAudioSampleRate)) return false;
    ESP_LOGI("doit_audio", "MAX98357A: 48 kHz, 16-bit stereo slots, software volume");
    return true;
}

bool DoitGamekitAudio::Write(const int16_t* pcm, size_t count) {
    if (!pcm || !count || !channel_) return false;
    const int gain = volume();
    // MAX98357A has no register interface. Scale mono PCM in software, then
    // duplicate into both slots so left/right/average strap choices all work.
    while (count) {
        const size_t chunk = std::min(count, size_t(320));
        for (size_t i = 0; i < chunk; ++i)
            stereo_[2 * i] = stereo_[2 * i + 1] = int32_t(pcm[i]) * gain / 100;
        const size_t bytes = chunk * 2 * sizeof(int16_t);
        size_t written = 0;
        const auto error = i2s_channel_write(channel_, stereo_, bytes, &written, 100);
        if (error != ESP_OK || written != bytes) return false;
        pcm += chunk;
        count -= chunk;
    }
    return true;
}
