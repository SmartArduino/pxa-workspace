#include "esp_mosaico_audio.h"

#include <algorithm>
#include <cstring>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <es8311_codec.h>

#include "esp_mosaico_config.h"
#include "esp_mosaico_audio_transfer.h"

namespace {
constexpr size_t kDmaFrames = mosaico_board::kAudioSampleRate / 50;
constexpr size_t kDmaDescriptors = 6;
}

bool EspMosaicoAudio::Check(int result, const char* stage) {
    init_stage_ = stage;
    init_error_ = result;
    if (result == ESP_OK) return true;
    ESP_LOGE("mosaico_audio", "Initialization failed: stage=%s code=%d", stage, result);
    return false;
}

bool EspMosaicoAudio::Initialize(i2c_master_bus_handle_t bus) {
    mutex_ = xSemaphoreCreateMutex();
    if (!Check(mutex_ ? ESP_OK : ESP_ERR_NO_MEM, "mutex")) return false;
    control_mutex_ = xSemaphoreCreateMutex();
    if (!Check(control_mutex_ ? ESP_OK : ESP_ERR_NO_MEM, "control_mutex")) return false;
    // Keep the speaker amplifier quiet while clocks and DAC registers settle.
    gpio_config_t amplifier = {};
    amplifier.pin_bit_mask = 1ULL << mosaico_board::kAudioAmplifierEnable;
    // Enable input readback as well, so diagnostics can verify the PA pin.
    amplifier.mode = GPIO_MODE_INPUT_OUTPUT;
    if (!Check(gpio_set_level(static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable), 0), "pa_disable") ||
        !Check(gpio_config(&amplifier), "pa_gpio")) return false;

    // IDF main's default macro currently has C++ designators out of order.
    i2s_chan_config_t channel_config = {};
    channel_config.id = I2S_NUM_0;
    channel_config.role = I2S_ROLE_MASTER;
    channel_config.dma_desc_num = kDmaDescriptors;
    // Match the Host's 320-sample/20-ms writes. With 240-frame descriptors,
    // each write leaves a partial buffer that can expire before its next fill.
    channel_config.dma_frame_num = kDmaFrames;
    channel_config.auto_clear = true;
    if (!Check(i2s_new_channel(&channel_config, &tx_channel_, nullptr), "i2s_channel"))
        return false;
    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(mosaico_board::kAudioSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = static_cast<gpio_num_t>(mosaico_board::kAudioMasterClock),
            .bclk = static_cast<gpio_num_t>(mosaico_board::kAudioBitClock),
            .ws = static_cast<gpio_num_t>(mosaico_board::kAudioWordSelect),
            .dout = static_cast<gpio_num_t>(mosaico_board::kAudioDataOut),
            .din = GPIO_NUM_NC,
        },
    };
    const i2s_event_callbacks_t callbacks = {.on_sent = OnDmaSent};
    if (!Check(i2s_channel_init_std_mode(tx_channel_, &std_config), "i2s_format") ||
        !Check(i2s_channel_register_event_callback(tx_channel_, &callbacks, this), "i2s_callback") ||
        !Check(i2s_channel_enable(tx_channel_), "i2s_enable"))
        return false;

    audio_codec_i2s_cfg_t i2s_config = {
        .port = I2S_NUM_0,
        .rx_handle = nullptr,
        .tx_handle = tx_channel_,
    };
    const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_config);
    const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0,
        .addr = static_cast<uint8_t>(mosaico_board::kAudioCodecAddress << 1),
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t* ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!Check(data_if && gpio_if && ctrl_if ? ESP_OK : ESP_ERR_NO_MEM, "codec_interfaces"))
        return false;
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC, // Enabled below after silent preroll.
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        // Match the Mosaico BSP's amplifier supply and DAC reference routing.
        .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
        .no_dac_ref = true,
        .mclk_div = 256,
    };
    const audio_codec_if_t* codec_if = es8311_codec_new(&codec_config);
    if (!Check(codec_if ? ESP_OK : ESP_FAIL, "es8311_create")) return false;
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    speaker_ = esp_codec_dev_new(&device_config);
    if (!Check(speaker_ ? ESP_OK : ESP_ERR_NO_MEM, "speaker_create")) return false;
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = mosaico_board::kAudioSampleRate,
        .mclk_multiple = 256,
    };
    if (!Check(esp_codec_dev_open(speaker_, &sample_info), "speaker_open")) return false;
    if (!Check(esp_codec_dev_set_out_mute(speaker_, true), "mute")) return false;
    if (!Check(SetVolume(volume()) ? ESP_OK : ESP_FAIL, "volume")) return false;
    // Fill all six DMA descriptors with silence, then let the DAC settle
    // before enabling the external amplifier and unmuting it.
    for (size_t remaining = kDmaDescriptors * kDmaFrames; remaining != 0;) {
        const size_t frames = std::min(remaining, kMaxWriteSamples);
        if (!Check(esp_codec_dev_write(speaker_, stereo_, frames * 2 * sizeof(int16_t)), "preroll"))
            return false;
        remaining -= frames;
    }
    vTaskDelay(pdMS_TO_TICKS(24));
    if (!Check(output_.Initialize([](void* context, const int16_t* pcm, size_t samples) {
            return static_cast<EspMosaicoAudio*>(context)->Write(pcm, samples);
        }, this, true) ? ESP_OK : ESP_FAIL, "host_output")) return false;
    if (!Check(gpio_set_level(static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable), 1), "pa_enable") ||
        !Check(esp_codec_dev_set_out_mute(speaker_, false), "unmute")) {
        (void)gpio_set_level(static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable), 0);
        return false;
    }
    ready_ = true;
    init_stage_ = "ready";
    ESP_LOGI("mosaico_audio", "ES8311 ready at %lu Hz",
             static_cast<unsigned long>(mosaico_board::kAudioSampleRate));
    return true;
}

bool EspMosaicoAudio::SetVolume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    if (speaker_ == nullptr || control_mutex_ == nullptr) return false;
    xSemaphoreTake(control_mutex_, portMAX_DELAY);
    // Match the Watcher's codec headroom at the highest UI volume setting.
    const esp_err_t result = esp_codec_dev_set_out_vol(speaker_,
        std::min<uint8_t>(percent, 95));
    if (result == ESP_OK) volume_.store(percent);
    xSemaphoreGive(control_mutex_);
    if (result != ESP_OK)
        ESP_LOGW("mosaico_audio", "Volume update failed: %s", esp_err_to_name(result));
    return result == ESP_OK;
}

bool EspMosaicoAudio::Write(const int16_t* pcm, size_t samples) {
    if (speaker_ == nullptr || mutex_ == nullptr || pcm == nullptr ||
        samples == 0 || samples > kMaxWriteSamples)
        return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    uint32_t peak = 0;
    for (size_t index = 0; index < samples; ++index) {
        stereo_[index * 2] = pcm[index];
        stereo_[index * 2 + 1] = pcm[index];
        const int32_t sample = pcm[index];
        peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
    }
    if (dma_progress_.Stalled(peak != 0, dma_nonzero_blocks_.load(std::memory_order_relaxed))) {
        ESP_LOGW("mosaico_audio", "Nonzero PCM is not reaching DMA; restarting output");
        if (!RestartOutput()) {
            xSemaphoreGive(mutex_);
            return false;
        }
    }
    // The codec wrapper discards bytes_written and collapses driver errors.
    // Use its configured I2S channel directly so short writes cannot pass as
    // success, and recover a stopped DMA channel before failing Host playback.
    const bool written = mosaico_board::SubmitAudioFrame(
        stereo_, samples * 2 * sizeof(int16_t),
        [this](const void* data, size_t bytes, size_t* count) {
            const esp_err_t result = i2s_channel_write(tx_channel_, data, bytes, count, 100);
            if (result == ESP_OK && *count == bytes) return true;
            write_errors_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW("mosaico_audio", "I2S write: %s bytes=%u/%u",
                esp_err_to_name(result), static_cast<unsigned>(*count), static_cast<unsigned>(bytes));
            return false;
        }, [this]() { return RestartOutput(); });
    xSemaphoreGive(mutex_);
    if (written) {
        writes_.fetch_add(1, std::memory_order_relaxed);
        if (peak) nonzero_frames_.fetch_add(1, std::memory_order_relaxed);
        uint32_t previous = peak_.load(std::memory_order_relaxed);
        while (previous < peak && !peak_.compare_exchange_weak(
            previous, peak, std::memory_order_relaxed)) {}
    }
    return written;
}

bool IRAM_ATTR EspMosaicoAudio::OnDmaSent(i2s_chan_handle_t, i2s_event_data_t* event, void* context) {
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "ISR counters must be lock-free");
    auto* self = static_cast<EspMosaicoAudio*>(context);
    self->dma_blocks_.fetch_add(1, std::memory_order_relaxed);
    // Called before auto-clear: these are samples actually consumed by DMA,
    // rather than samples merely accepted by the software write path.
    const auto* pcm = static_cast<const int16_t*>(event->dma_buf);
    for (size_t i = 0; i < event->size / sizeof(int16_t); ++i) {
        if (pcm[i] == 0) continue;
        self->dma_nonzero_blocks_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    return false;
}

bool EspMosaicoAudio::RestartOutput() {
    const auto pa = static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable);
    (void)gpio_set_level(pa, 0);
    esp_err_t result = i2s_channel_disable(tx_channel_);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
    static const int16_t silence[kDmaFrames * 2] = {};
    for (size_t i = 0; i < kDmaDescriptors; ++i) {
        size_t loaded = 0;
        result = i2s_channel_preload_data(tx_channel_, silence, sizeof(silence), &loaded);
        if (result != ESP_OK || loaded != sizeof(silence)) return false;
    }
    result = i2s_channel_enable(tx_channel_);
    if (result != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(8));
    if (gpio_set_level(pa, 1) != ESP_OK) return false;
    recoveries_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGW("mosaico_audio", "I2S DMA restarted after a write fault");
    return true;
}

void EspMosaicoAudio::PublishDiagnostics() {
    if (!ready_) {
        ESP_LOGW("mosaico_audio", "audio ready=0 stage=%s code=%d", init_stage_, init_error_);
        return;
    }
    int codec_volume = -1, mute_reg = -1, volume_reg = -1, reference_reg = -1;
    // Codec I2C transactions can take 100 ms on failure. They must never hold
    // the PCM write lock while the I2S DMA ring continues draining.
    if (xSemaphoreTake(control_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) return;
    (void)esp_codec_dev_get_out_vol(speaker_, &codec_volume);
    int result = esp_codec_dev_read_reg(speaker_, 0x31, &mute_reg);
    if (result == ESP_OK) result = esp_codec_dev_read_reg(speaker_, 0x32, &volume_reg);
    if (result == ESP_OK) result = esp_codec_dev_read_reg(speaker_, 0x44, &reference_reg);
    xSemaphoreGive(control_mutex_);
    ESP_LOGI("mosaico_audio", "audio ready=1 volume=%u codec_volume=%d pa=%d writes=%lu nonzero=%lu peak=%lu errors=%lu reg31=0x%x reg32=0x%x reg44=0x%x read_status=%d",
        volume(), codec_volume, gpio_get_level(static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable)),
        static_cast<unsigned long>(writes_.load()), static_cast<unsigned long>(nonzero_frames_.load()),
        static_cast<unsigned long>(peak_.exchange(0)), static_cast<unsigned long>(write_errors_.load()),
        mute_reg, volume_reg, reference_reg, result);
    PxaAudioOutput::MusicStats stats;
    ESP_LOGI("mosaico_audio", "dma blocks=%lu nonzero=%lu recoveries=%lu",
        static_cast<unsigned long>(dma_blocks_.load()),
        static_cast<unsigned long>(dma_nonzero_blocks_.load()),
        static_cast<unsigned long>(recoveries_.load()));
    if (output_.GetMusicStats(&stats)) {
        ESP_LOGI("mosaico_audio", "music consumed=%llu underruns=%llu recovered=%llu missing=%llu decode_max_us=%llu read_max_us=%llu",
            static_cast<unsigned long long>(stats.buffer.consumed_samples),
            static_cast<unsigned long long>(stats.buffer.underruns),
            static_cast<unsigned long long>(stats.buffer.recoveries),
            static_cast<unsigned long long>(stats.buffer.missing_samples),
            static_cast<unsigned long long>(stats.decode_max_us),
            static_cast<unsigned long long>(stats.read_max_us));
    }
}
