#include "pxa/pxa_audio_capture_input.h"
#include "pxa/pxa_audio_capture.h"
#include "pxa_codec_memory.h"
#include "pxa_esp_resource_memory.h"

#include <cstring>
#include <new>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_opus_enc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
constexpr size_t kFrameSamples = 320;
// Cover bounded storage stalls while a recorder drains encoded packets.
// Memory remains fixed, and closing a generation discards its backlog.
constexpr unsigned kPacketCount = 64;
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
    StaticTask_t task_storage = {};
    StackType_t* stack = nullptr;
    QueueHandle_t queue = nullptr;
    StaticQueue_t queue_storage = {};
    uint8_t* queue_bytes = nullptr;
    // Only creation and reclamation use this mutex. ADC, encoder, and queue
    // operations run outside it. The spinlock protects publication and borrows.
    StaticSemaphore_t lifecycle_storage = {};
    SemaphoreHandle_t lifecycle = nullptr;
    esp_timer_handle_t reap_timer = nullptr;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    uint32_t generation = 0, requested = 0, fault_generation = 0;
    unsigned borrowers = 0;
    bool shutdown = false, parked = false;

    // Caller holds lock. Reclamation waits for users of the task or queue.
    TaskHandle_t BorrowTask() {
        if (task) ++borrowers;
        return task;
    }

    void ReleaseBorrow() {
        portENTER_CRITICAL(&lock);
        --borrowers;
        portEXIT_CRITICAL(&lock);
    }

    void Notify(TaskHandle_t handle) {
        if (handle) {
            xTaskNotifyGive(handle);
            ReleaseBorrow();
        }
    }

    bool Start() {
        const auto* audio = pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_AUDIO);
        stack = static_cast<StackType_t*>(pxa_memory_allocate(audio, kStackBytes));
        if (stack) queue_bytes = static_cast<uint8_t*>(pxa_memory_allocate(audio, kPacketCount * sizeof(Packet)));
        if (queue_bytes) queue = xQueueCreateStatic(kPacketCount, sizeof(Packet), queue_bytes, &queue_storage);
        TaskHandle_t created = nullptr;
        if (queue)
            created = xTaskCreateStatic(Task, "pxa_capture", kStackBytes, this, 5, stack, &task_storage);
        if (!created) {
            ReleaseStorage();
            return false;
        }
        portENTER_CRITICAL(&lock);
        task = created;
        portEXIT_CRITICAL(&lock);
        return true;
    }

    void ReleaseStorage() {
        if (queue) vQueueDelete(queue);
        queue = nullptr;
        pxa_memory_release(queue_bytes); queue_bytes = nullptr;
        pxa_memory_release(stack); stack = nullptr;
    }

    bool Reap() {
        if (xSemaphoreTake(lifecycle, 0) != pdTRUE) return false;
        portENTER_CRITICAL(&lock);
        const bool wanted = requested != 0;
        TaskHandle_t retired = nullptr;
        if (!wanted && task && parked && !borrowers) {
            retired = task;
            task = nullptr;
            parked = false;
        }
        const bool done = wanted || !task;
        portEXIT_CRITICAL(&lock);
        if (retired) {
            // IDF only deletes a static task synchronously when it is off
            // every core. A suspended task can still be the remote current TCB.
            vTaskSuspend(retired);
            for (int core = 0; core < configNUMBER_OF_CORES; ++core)
                while (xTaskGetCurrentTaskHandleForCore(core) == retired) taskYIELD();
            vTaskDelete(retired);
            ReleaseStorage();
        }
        xSemaphoreGive(lifecycle);
        return done;
    }

    // One shared esp_timer service handles idle capture cleanup. No additional
    // FreeRTOS task or Opus stack is needed to reclaim the parked worker.
    void ScheduleReap() { (void)esp_timer_start_once(reap_timer, 20000); }
    static void OnReap(void* context) {
        auto* state = static_cast<State*>(context);
        if (!state->Reap()) state->ScheduleReap();
    }
};

bool PxaAudioCaptureInput::Initialize(Control control, Read read, void* context) {
    if (state_ || !control || !read || pxa_esp_resource_memory_initialize() != PXA_STATUS_OK)
        return false;
    const auto* metadata = pxa_esp_device_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_METADATA);
    void* memory = pxa_memory_allocate(metadata, sizeof(State));
    if (!memory) return false;
    auto* state = new (memory) State;
    state->control = control; state->read = read; state->context = context;
    state->lifecycle = xSemaphoreCreateMutexStatic(&state->lifecycle_storage);
    esp_timer_create_args_t timer = {};
    timer.callback = State::OnReap;
    timer.arg = state;
    timer.name = "pxa_cap_idle";
    if (!state->lifecycle || esp_timer_create(&timer, &state->reap_timer) != ESP_OK) {
        if (state->lifecycle) vSemaphoreDelete(state->lifecycle);
        state->~State(); pxa_memory_release(state);
        return false;
    }
    state_ = state;
    pxa_esp_audio_set_capture_source(state, 20, Open, ReadPacket, Close);
    return true;
}

pxa_status_t PxaAudioCaptureInput::Open(void* context) {
    auto* state = static_cast<State*>(context);
    xSemaphoreTake(state->lifecycle, portMAX_DELAY);
    portENTER_CRITICAL(&state->lock);
    if (state->shutdown || state->requested) {
        const auto status = state->shutdown ? PXA_STATUS_UNAVAILABLE : PXA_STATUS_BUSY;
        portEXIT_CRITICAL(&state->lock);
        xSemaphoreGive(state->lifecycle);
        return status;
    }
    if (++state->generation == 0) ++state->generation;
    const uint32_t generation = state->generation;
    state->requested = generation;
    state->fault_generation = 0;
    portEXIT_CRITICAL(&state->lock);

    const bool ready = state->task || state->Start();
    portENTER_CRITICAL(&state->lock);
    const bool current = state->requested == generation;
    if (!ready && current) state->requested = 0;
    pxa_status_t status = ready ? PXA_STATUS_OK : PXA_STATUS_RESOURCE_LIMIT;
    if (!current) status = PXA_STATUS_CANCELLED;
    if (state->shutdown) status = PXA_STATUS_UNAVAILABLE;
    TaskHandle_t task = state->BorrowTask();
    portEXIT_CRITICAL(&state->lock);
    xSemaphoreGive(state->lifecycle);
    state->Notify(task);
    if (task && !current) state->ScheduleReap();
    return status;
}

int32_t PxaAudioCaptureInput::ReadPacket(void* context, uint8_t* output, size_t capacity) {
    auto* state = static_cast<State*>(context);
    if (!output || capacity < PXA_AUDIO_CAPTURE_MAX_PACKET) return PXA_STATUS_INVALID_ARGUMENT;
    portENTER_CRITICAL(&state->lock);
    const uint32_t generation = state->requested;
    const bool failed = generation && state->fault_generation == generation;
    QueueHandle_t queue = generation && !failed && state->task ? state->queue : nullptr;
    if (queue) ++state->borrowers;
    portEXIT_CRITICAL(&state->lock);
    if (!generation) return PXA_STATUS_UNAVAILABLE;
    if (failed) return PXA_STATUS_IO_ERROR;
    if (!queue) return PXA_STATUS_WOULD_BLOCK;
    Packet packet;
    int32_t result = PXA_STATUS_WOULD_BLOCK;
    for (unsigned i = 0; i < kPacketCount; ++i) {
        if (xQueueReceive(queue, &packet, 0) != pdTRUE) break;
        if (packet.generation != generation) continue;
        std::memcpy(output, packet.data, packet.size);
        result = packet.size;
        break;
    }
    state->ReleaseBorrow();
    return result;
}

void PxaAudioCaptureInput::Close(void* context) {
    auto* state = static_cast<State*>(context);
    portENTER_CRITICAL(&state->lock);
    state->requested = 0;
    TaskHandle_t task = state->shutdown ? nullptr : state->BorrowTask();
    portEXIT_CRITICAL(&state->lock);
    state->Notify(task);
    if (task) state->ScheduleReap();
}

void PxaAudioCaptureInput::StopForShutdown() {
    auto* state = state_;
    if (!state) return;
    portENTER_CRITICAL(&state->lock);
    TaskHandle_t task = state->shutdown ? nullptr : state->BorrowTask();
    state->shutdown = true;
    state->requested = 0;
    portEXIT_CRITICAL(&state->lock);
    state->Notify(task);
    // All shutdown callers wait for the ADC, encoder, callbacks and the final
    // release of the recording stack and ring. Registered source stays closed.
    while (!state->Reap()) vTaskDelay(1);
}

void PxaAudioCaptureInput::Task(void* context) {
    auto* state = static_cast<State*>(context);
    // Start() publishes the task and queue before the first wakeup.
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
        if (requested && !shutdown && !fault) state->parked = false;
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
                portENTER_CRITICAL(&state->lock);
                state->parked = true;
                portEXIT_CRITICAL(&state->lock);
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
}
