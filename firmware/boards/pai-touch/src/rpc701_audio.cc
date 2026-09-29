#include "rpc701_audio.h"
#include "pai_touch_config.h"
#include <algorithm>
#include <esp_event.h>
#include <esp_log.h>
#include <rpc_701.h>
#include <rpc_adapter.h>
#include <rpc_wrap.h>
namespace { constexpr char kTag[] = "Rpc701Audio"; }
bool Rpc701Audio::Initialize() {
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
    const rpc_adapter_config_t config = {
        .uart_num = PAI_JL701_UART,
        .tx_pin = PAI_JL701_TX,
        .rx_pin = PAI_JL701_RX,
        .baud_rate = PAI_JL701_BAUD,
        .configure_pa = true,
        .pa_io = PAI_JL701_PA_IO,
        .pa_en_level = PAI_JL701_PA_LEVEL,
    };
    rpc_adapter_init(&config);
    if (!rpc_adapter_is_initialized()) {
        ESP_LOGE(kTag, "RPC701 transport initialization failed");
        return false;
    }
    audio_opus_frame_flush();

    return output_.Initialize(Write, this);
}
void Rpc701Audio::SetVolume(uint8_t percent) {
    volume_.store(std::min<uint8_t>(percent, 100));
    volume_dirty_.store(true);
}
size_t Rpc701Audio::ReadOpus(uint8_t* output, size_t capacity) {
    if (!output || !capacity) return 0;
    int size = audio_opus_frame_read(output, capacity);
    return size > 0 ? static_cast<size_t>(size) : 0;
}
bool Rpc701Audio::Write(void* context, const int16_t* pcm, size_t samples) {
    auto* self = static_cast<Rpc701Audio*>(context);
    if (!rpc_adapter_is_slave_ready()) return false;
    if (self->volume_dirty_.exchange(false) &&
        rpc_music_volume_set(self->volume_.load()) != RPC_ERR_SUCCESS)
        self->volume_dirty_.store(true);
    TickType_t last = xTaskGetTickCount();
    for (size_t offset = 0; offset < samples; offset += 160) {
        const size_t count = std::min<size_t>(160, samples - offset);
        if (rpc_sen_audio_pcm(reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm + offset)),
                              count * sizeof(int16_t)) != RPC_ERR_SUCCESS) return false;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    }
    return true;
}
