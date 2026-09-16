#include "rpc701_audio.h"

#include "pai_touch_config.h"

#include <algorithm>
#include <cstring>
#include <driver/uart.h>
#include <esp_event.h>
#include <esp_log.h>
#include <pxa/pxa_host.h>
#include <rpc_701.h>
#include <rpc_adapter.h>
#include <rpc_wrap.h>

namespace {
constexpr char kTag[] = "Rpc701Audio";
constexpr size_t kRpcPcmSamples = 160;
}

bool Rpc701Audio::Initialize() {
    esp_err_t result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Cannot create event loop: %s", esp_err_to_name(result));
        return false;
    }

    output_queue_ = xQueueCreate(12, sizeof(OutputFrame));
    volume_queue_ = xQueueCreate(1, sizeof(uint8_t));
    if (output_queue_ == nullptr || volume_queue_ == nullptr) {
        ESP_LOGE(kTag, "Cannot allocate RPC701 audio queues");
        return false;
    }

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

    if (xTaskCreate(AudioTask, "rpc701_audio", 4096, this, 6, &task_) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(kTag, "Cannot create RPC701 audio task");
        return false;
    }
    pxa_host_set_audio_sink(Submit, Flush, this);
    SetVolume(volume_.load());
    if (rpc_adapter_is_slave_ready()) {
        ESP_LOGI(kTag,
                 "RPC701 ready: input=Opus queue, output=16 kHz mono PCM, UART2=2 Mbps");
    } else {
        ESP_LOGW(kTag,
                 "RPC701 UART started; waiting for the slave handshake retry");
    }
    return true;
}

void Rpc701Audio::SetVolume(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    volume_.store(percent);
    if (volume_queue_ != nullptr) xQueueOverwrite(volume_queue_, &percent);
}

size_t Rpc701Audio::ReadOpus(uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity == 0) return 0;
    const int size = audio_opus_frame_read(output, capacity);
    return size > 0 ? static_cast<size_t>(size) : 0;
}

bool Rpc701Audio::Submit(void* context, uint8_t voice, const int16_t* pcm,
                         size_t samples) {
    auto* self = static_cast<Rpc701Audio*>(context);
    if (self == nullptr || self->output_queue_ == nullptr || pcm == nullptr ||
        samples == 0 || samples > kFrameSamples) {
        return false;
    }
    OutputFrame frame = {};
    frame.samples = static_cast<uint16_t>(samples);
    std::memcpy(frame.pcm, pcm, samples * sizeof(*pcm));
    (void)voice;
    return xQueueSend(self->output_queue_, &frame, 0) == pdTRUE;
}

void Rpc701Audio::Flush(void* context, uint8_t voice) {
    auto* self = static_cast<Rpc701Audio*>(context);
    (void)voice;
    if (self != nullptr && self->output_queue_ != nullptr) {
        xQueueReset(self->output_queue_);
    }
}

void Rpc701Audio::AudioTask(void* context) {
    static_cast<Rpc701Audio*>(context)->Run();
    vTaskDelete(nullptr);
}

void Rpc701Audio::Run() {
    OutputFrame frame;
    TickType_t last_send = xTaskGetTickCount();
    for (;;) {
        uint8_t requested_volume;
        if (xQueueReceive(volume_queue_, &requested_volume, 0) == pdTRUE) {
            if (rpc_adapter_is_slave_ready()) {
                const int result = rpc_music_volume_set(requested_volume);
                if (result != RPC_ERR_SUCCESS) {
                    ESP_LOGW(kTag, "Volume %u rejected: %d",
                             static_cast<unsigned>(requested_volume), result);
                }
            } else {
                xQueueOverwrite(volume_queue_, &requested_volume);
            }
        }

        if (xQueueReceive(output_queue_, &frame, pdMS_TO_TICKS(10)) != pdTRUE) {
            continue;
        }
        for (size_t offset = 0; offset < frame.samples; offset += kRpcPcmSamples) {
            const size_t chunk = std::min(kRpcPcmSamples,
                                          static_cast<size_t>(frame.samples) - offset);
            if (rpc_adapter_is_slave_ready()) {
                const int result = rpc_sen_audio_pcm(
                    reinterpret_cast<uint8_t*>(frame.pcm + offset),
                    chunk * sizeof(int16_t));
                if (result != RPC_ERR_SUCCESS) {
                    ESP_LOGW(kTag, "PCM transfer failed: %d", result);
                }
            }
            vTaskDelayUntil(&last_send, pdMS_TO_TICKS(10));
        }
    }
}
