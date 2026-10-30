#include "pxa/pxa_audio_capture_input.h"
#include "pxa/pxa_audio_capture.h"
#include "pxa/pxa_worker_notifications.h"
#include "pxa_codec_memory.h"
#include "pxa_esp_resource_memory.h"

#include <atomic>
#include <cstring>
#include <new>
#include <esp_log.h>
#include <esp_opus_enc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {
constexpr size_t kFrameSamples = 320;
constexpr unsigned kPacketCount = 4;
// The native Opus encoder uses a deep stack even at complexity zero. Keep it
// on budgeted PSRAM rather than consuming the Host's internal SRAM stack.
constexpr size_t kStackBytes = 65536;
struct Packet {
    uint32_t generation;
    uint16_t size;
    uint8_t data[PXA_AUDIO_CAPTURE_MAX_PACKET];
};
}

struct PxaAudioCaptureInput::State {
    Control control;
    Read read;
    void* context;
    TaskHandle_t task = nullptr;
    PxaWorkerNotifications notifications;
    StaticTask_t task_storage = {};
    StackType_t* stack = nullptr;
    QueueHandle_t queue = nullptr;
    StaticQueue_t queue_storage = {};
    uint8_t* queue_bytes = nullptr;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    uint32_t generation = 0, requested = 0, fault_generation = 0;
    bool shutdown = false;
    std::atomic<bool> busy{false};
};

bool PxaAudioCaptureInput::Initialize(Control control, Read read, void* context) {
    if (state_ || !control || !read || pxa_esp_resource_memory_initialize() != PXA_STATUS_OK)
        return false;
    const auto* metadata = pxa_esp_device_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_METADATA);
    const auto* audio = pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_AUDIO);
    void* memory = pxa_memory_allocate(metadata, sizeof(State));
    if (!memory) return false;
    auto* state = new (memory) State;
    state->control = control; state->read = read; state->context = context;
    state->stack = static_cast<StackType_t*>(pxa_memory_allocate(audio, kStackBytes));
    state->queue_bytes = static_cast<uint8_t*>(pxa_memory_allocate(audio, kPacketCount * sizeof(Packet)));
    if (state->stack && state->queue_bytes)
        state->queue = xQueueCreateStatic(kPacketCount, sizeof(Packet), state->queue_bytes, &state->queue_storage);
    if (state->queue)
        state->task = xTaskCreateStatic(Task, "pxa_capture", kStackBytes, state, 5,
                                        state->stack, &state->task_storage);
    if (!state->task) {
        if (state->queue) vQueueDelete(state->queue);
        pxa_memory_release(state->queue_bytes); pxa_memory_release(state->stack);
        state->~State(); pxa_memory_release(state);
        return false;
    }
    state_ = state;
    pxa_esp_audio_set_capture_source(state, 20, Open, ReadPacket, Close);
    return true;
}

pxa_status_t PxaAudioCaptureInput::Open(void* context) {
    auto* state = static_cast<State*>(context);
    portENTER_CRITICAL(&state->lock);
    if (state->shutdown || state->requested) {
        const auto status = state->shutdown ? PXA_STATUS_UNAVAILABLE : PXA_STATUS_BUSY;
        portEXIT_CRITICAL(&state->lock);
        return status;
    }
    if (++state->generation == 0) ++state->generation;
    state->requested = state->generation;
    state->fault_generation = 0;
    portEXIT_CRITICAL(&state->lock);
    (void)state->notifications.Notify(state->task);
    return PXA_STATUS_OK;
}

int32_t PxaAudioCaptureInput::ReadPacket(void* context, uint8_t* output, size_t capacity) {
    auto* state = static_cast<State*>(context);
    if (!output || capacity < PXA_AUDIO_CAPTURE_MAX_PACKET) return PXA_STATUS_INVALID_ARGUMENT;
    portENTER_CRITICAL(&state->lock);
    const uint32_t generation = state->requested;
    const bool failed = generation && state->fault_generation == generation;
    portEXIT_CRITICAL(&state->lock);
    if (!generation) return PXA_STATUS_UNAVAILABLE;
    if (failed) return PXA_STATUS_IO_ERROR;
    Packet packet;
    for (unsigned i = 0; i < kPacketCount; ++i) {
        if (xQueueReceive(state->queue, &packet, 0) != pdTRUE) break;
        if (packet.generation != generation) continue;
        std::memcpy(output, packet.data, packet.size);
        return packet.size;
    }
    return PXA_STATUS_WOULD_BLOCK;
}

void PxaAudioCaptureInput::Close(void* context) {
    auto* state = static_cast<State*>(context);
    portENTER_CRITICAL(&state->lock);
    state->requested = 0;
    const bool shutdown = state->shutdown;
    portEXIT_CRITICAL(&state->lock);
    if (!shutdown) (void)state->notifications.Notify(state->task);
}

void PxaAudioCaptureInput::StopForShutdown() {
    auto* state = state_;
    if (!state) return;
    portENTER_CRITICAL(&state->lock);
    const bool first = !state->shutdown;
    state->shutdown = true;
    state->requested = 0;
    portEXIT_CRITICAL(&state->lock);
    if (first) (void)state->notifications.Notify(state->task);
    // Concurrent shutdown callers all wait for ADC/encoder closure. Only the
    // first needs to wake the worker; its gate also protects task deletion.
    // A PCM read has a bounded hardware timeout. A generation change already
    // blocks delivery; wait for worker-side ADC/encoder closure before power-off.
    while (state->busy.load(std::memory_order_acquire)) vTaskDelay(1);
}

void PxaAudioCaptureInput::Task(void* context) {
    auto* state = static_cast<State*>(context);
    PxaCodecMemoryScope memory(pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_AUDIO));
    void* encoder = nullptr;
    uint32_t active = 0;
    bool input_open = false;
    int16_t pcm[kFrameSamples];
    size_t filled = 0;
    for (;;) {
        portENTER_CRITICAL(&state->lock);
        const uint32_t requested = state->requested;
        const bool shutdown = state->shutdown;
        const bool fault = requested && requested == state->fault_generation;
        // Serialized with shutdown, including the interval before ADC open.
        if (requested && !shutdown && !fault) state->busy.store(true, std::memory_order_release);
        portEXIT_CRITICAL(&state->lock);
        if (!requested || shutdown || fault || requested != active) {
            if (encoder) {
                esp_opus_enc_close(encoder); encoder = nullptr;
                ESP_LOGI("PxaCapture", "Encoder stopped; worker minimum free stack=%u bytes",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
            if (input_open) { (void)state->control(state->context, false); input_open = false; }
            active = 0; filled = 0;
            if (!requested || shutdown || fault) {
                if (shutdown) break;
                state->busy.store(false, std::memory_order_release);
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            input_open = state->control(state->context, true);
            esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
            config.sample_rate = 16000; config.channel = 1; config.bits_per_sample = 16;
            config.bitrate = 16000; config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
            config.complexity = 0;
            int in_bytes = 0, out_bytes = 0;
            if (!input_open || esp_opus_enc_open(&config, sizeof(config), &encoder) != ESP_AUDIO_ERR_OK ||
                esp_opus_enc_get_frame_size(encoder, &in_bytes, &out_bytes) != ESP_AUDIO_ERR_OK ||
                in_bytes != int(sizeof(pcm)) || out_bytes <= 0 || out_bytes > int(PXA_AUDIO_CAPTURE_MAX_PACKET)) {
                portENTER_CRITICAL(&state->lock);
                state->fault_generation = requested;
                portEXIT_CRITICAL(&state->lock);
                continue;
            }
            active = requested;
        }
        const int32_t count = state->read(state->context, pcm + filled, kFrameSamples - filled);
        if (count == PXA_STATUS_WOULD_BLOCK || count == 0) { vTaskDelay(1); continue; }
        if (count < 0 || size_t(count) > kFrameSamples - filled) {
            portENTER_CRITICAL(&state->lock); state->fault_generation = active; portEXIT_CRITICAL(&state->lock);
            continue;
        }
        filled += count;
        if (filled != kFrameSamples) continue;
        Packet packet = {}; packet.generation = active;
        esp_audio_enc_in_frame_t input = {reinterpret_cast<uint8_t*>(pcm), sizeof(pcm)};
        esp_audio_enc_out_frame_t output = {packet.data, sizeof(packet.data), 0, 0};
        if (esp_opus_enc_process(encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
            !output.encoded_bytes || output.encoded_bytes > sizeof(packet.data)) {
            portENTER_CRITICAL(&state->lock); state->fault_generation = active; portEXIT_CRITICAL(&state->lock);
            continue;
        }
        filled = 0; packet.size = output.encoded_bytes;
        portENTER_CRITICAL(&state->lock);
        const bool current = state->requested == active && !state->shutdown;
        portEXIT_CRITICAL(&state->lock);
        if (current && xQueueSendToBack(state->queue, &packet, 0) != pdTRUE) {
            Packet discarded;
            (void)xQueueReceive(state->queue, &discarded, 0);
            (void)xQueueSendToBack(state->queue, &packet, 0);
        }
    }
    state->notifications.Stop();
    state->busy.store(false, std::memory_order_release);
    // Board-lifetime storage remains valid; no callback can reopen after shutdown.
    vTaskDelete(nullptr);
}
