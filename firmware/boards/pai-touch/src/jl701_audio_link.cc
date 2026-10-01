#include "jl701_audio_link.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <soc/uart_reg.h>

#include "rpc_701.h"
#include "rpc_wrap.h"

namespace {

constexpr char kTag[] = "Jl701AudioLink";

/* The JL701 decoder drains one 320-byte packet (160 mono samples at 16 kHz)
 * every 10 ms. The UART could burst far ahead of that, which fills and starves
 * the coprocessor receive buffer, so the link paces its writes instead. */
constexpr size_t kPcmPacketSamples = 160;
constexpr int kPcmPacketIntervalMs = 10;

constexpr int kHeartbeatIntervalMs = 2000;
constexpr int32_t kDefaultKeepAliveMs = 4000;
/* How long the link may stay down before the wake line is pulsed again. */
constexpr int32_t kReinitWakeAfterMs = 6000;
/* Receive-error reports are repeated at most this often. */
constexpr int64_t kUartErrorLogIntervalUs = 10 * 1000 * 1000;

/* The coprocessor streams audio without flow control, so the driver ring
 * buffer needs headroom for a burst while the receive task is parsing. */
constexpr int kUartDriverRxBufferSize = 16384;
constexpr int kRxChunkSize = 2048;
constexpr int kUartTxBufferSize = 1024;
constexpr int kUartEventQueueSize = 32;
constexpr uint32_t kRxTaskStackSize = 4096;
/* Above the display and audio tasks: a late drain costs whole frames. */
constexpr UBaseType_t kRxTaskPriority = 12;
constexpr uint32_t kHeartbeatTaskStackSize = 4096;
constexpr UBaseType_t kHeartbeatTaskPriority = 8;

/* The coprocessor samples its RX line while powered down; holding TX low is
 * what wakes it, so the pulse has to outlast its wake detector. */
constexpr uint32_t kWakePulseMs = 1000;

struct LinkState {
    jl701_audio_link_config_t config = {};
    QueueHandle_t uart_events = nullptr;
    TaskHandle_t rx_task = nullptr;
    TaskHandle_t heartbeat_task = nullptr;
    bool started = false;
    std::atomic<bool> ready{false};
    std::atomic<uint8_t> volume{70};
    std::atomic<bool> volume_dirty{true};
    std::atomic<int32_t> keep_alive_ms{kDefaultKeepAliveMs};
};

LinkState s_link;
int64_t s_last_uart_error_us = 0;

int UartWrite(const uint8_t* data, size_t length, void* context) {
    (void)context;
    return uart_write_bytes(s_link.config.uart_num, data, length);
}

/* Pulls TX low to wake the coprocessor, then hands the pin back to the UART. */
void WakeSlave() {
    ESP_LOGI(kTag, "Pulling the JL701 TX line low to wake the coprocessor");
    gpio_reset_pin(s_link.config.tx_pin);
    gpio_set_direction(s_link.config.tx_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(s_link.config.tx_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(kWakePulseMs));
    ESP_ERROR_CHECK(uart_set_pin(s_link.config.uart_num, s_link.config.tx_pin,
                                 s_link.config.rx_pin, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

/* Sends the handshake that configures keep-alive, the remote amplifier pin and
 * the audio codec format. The reply arrives through the UART receive task, so
 * that task must already be running. */
int InitializeSlave(bool wake_first) {
    if (wake_first) {
        WakeSlave();
    }

    vb_init_conf_t conf = {};
    conf.keep_alive_ms = kDefaultKeepAliveMs;
    conf.pa_io = s_link.config.pa_io;
    conf.pa_en_level = s_link.config.pa_en_level;
    conf.coder_fmt = CODEC_OPUS_20MS;
    conf.decoder_fmt = CODEC_OPUS_20MS;
    conf.debug_uart_io = -1;

    const int result = rpc_vb_init(&conf);
    if (result != RPC_ERR_SUCCESS) {
        ESP_LOGW(kTag, "JL701 rpc_vb_init failed: %d", result);
        s_link.ready.store(false);
        return result;
    }

    if (conf.keep_alive_ms > 0) {
        s_link.keep_alive_ms.store(conf.keep_alive_ms);
    }
    /* The coprocessor booted again, so the level has to be re-sent. */
    s_link.volume_dirty.store(true);
    s_link.ready.store(true);
    ESP_LOGI(kTag, "JL701 audio ready on UART%d at %d baud",
             static_cast<int>(s_link.config.uart_num),
             s_link.config.baud_rate);
    return RPC_ERR_SUCCESS;
}

void HandleUartError(const char* reason) {
    /* The coprocessor dumps a backlog of audio frames after it wakes, which
     * can outrun the driver ring buffer. The parser is resynchronised here and
     * nothing on the audio output path depends on those frames, so repeated
     * reports are only noise. */
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_uart_error_us >= kUartErrorLogIntervalUs) {
        s_last_uart_error_us = now_us;
        ESP_LOGW(kTag, "JL701 UART %s", reason);
    }
    rpc_701_reset_rx();
}

void RxTask(void* context) {
    (void)context;
    auto* buffer = static_cast<uint8_t*>(std::malloc(kRxChunkSize));
    if (buffer == nullptr) {
        ESP_LOGE(kTag, "JL701 UART receive buffer allocation failed");
        vTaskDelete(nullptr);
        return;
    }

    uart_event_t event;
    while (xQueueReceive(s_link.uart_events, &event, portMAX_DELAY) == pdTRUE) {
        switch (event.type) {
            case UART_DATA: {
                /* Drain what the driver holds before parsing. The coprocessor
                 * streams audio continuously, so a single read per event lets
                 * the driver ring buffer run over. */
                size_t remaining = event.size;
                while (remaining > 0) {
                    const size_t want = std::min<size_t>(remaining, kRxChunkSize);
                    const int length = uart_read_bytes(
                        s_link.config.uart_num, buffer, want, pdMS_TO_TICKS(20));
                    if (length <= 0) {
                        break;
                    }
                    remaining -= static_cast<size_t>(length);
                    rpc_701_feed_rx(buffer, static_cast<size_t>(length));
                }
                break;
            }
            case UART_BREAK:
                uart_clear_intr_status(
                    s_link.config.uart_num,
                    UART_RXFIFO_FULL_INT_CLR | UART_RXFIFO_TOUT_INT_CLR |
                        UART_BRK_DET_INT_CLR);
                HandleUartError("break");
                break;
            case UART_FIFO_OVF:
                HandleUartError("FIFO overflow");
                break;
            case UART_BUFFER_FULL:
                HandleUartError("buffer full");
                break;
            default:
                break;
        }
    }

    std::free(buffer);
    vTaskDelete(nullptr);
}

/* Keeps the coprocessor alive and brings it back. The JL701 drops the link
 * (and then sleeps) when keep_alive_ms passes without a heartbeat, so this
 * task both feeds it and re-runs the wake pulse plus handshake whenever the
 * link is down. Audio simply stops flowing while it is not ready. */
void HeartbeatTask(void* context) {
    (void)context;
    /* Let the handshake from start() settle before probing it. */
    vTaskDelay(pdMS_TO_TICKS(kHeartbeatIntervalMs));

    int32_t failed_ms = 0;
    int32_t not_ready_ms = 0;
    while (true) {
        if (s_link.ready.load()) {
            if (rpc_heartbeat() == RPC_ERR_SUCCESS) {
                if (failed_ms > 0) {
                    ESP_LOGI(kTag, "JL701 heartbeat recovered");
                    failed_ms = 0;
                }
            } else {
                failed_ms += kHeartbeatIntervalMs;
                const int32_t keep_alive_ms = s_link.keep_alive_ms.load();
                ESP_LOGW(kTag, "JL701 heartbeat failed (%ld/%ld ms)",
                         static_cast<long>(failed_ms),
                         static_cast<long>(keep_alive_ms));
                if (failed_ms >= keep_alive_ms) {
                    failed_ms = 0;
                    not_ready_ms = 0;
                    s_link.ready.store(false);
                    ESP_LOGW(kTag, "JL701 unresponsive, reinitializing");
                }
            }
        } else {
            /* Booting after a wake pulse needs a few seconds before the
             * coprocessor answers, so only re-assert the wake line once the
             * link has stayed down instead of pulsing it on every retry. */
            const bool wake_first = not_ready_ms >= kReinitWakeAfterMs;
            if (InitializeSlave(wake_first) == RPC_ERR_SUCCESS) {
                not_ready_ms = 0;
            } else if (wake_first) {
                not_ready_ms = 0;
            } else {
                not_ready_ms += kHeartbeatIntervalMs;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kHeartbeatIntervalMs));
    }
}

}  // namespace

bool jl701_audio_link_start(const jl701_audio_link_config_t* config) {
    if (config == nullptr) {
        ESP_LOGE(kTag, "JL701 audio link needs a configuration");
        return false;
    }
    if (s_link.started) {
        ESP_LOGW(kTag, "JL701 audio link is already started");
        return false;
    }

    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Failed to create the default event loop: %s",
                 esp_err_to_name(loop));
        return false;
    }
    s_link.config = *config;

    /* Assignment instead of a full initializer: uart_config_t grew fields
     * between IDF releases, and the remaining ones stay at their defaults
     * (source_clk keeps the driver's APB clock). */
    uart_config_t uart_config = {};
    uart_config.baud_rate = s_link.config.baud_rate;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    ESP_ERROR_CHECK(uart_driver_install(s_link.config.uart_num,
                                        kUartDriverRxBufferSize,
                                        kUartTxBufferSize, kUartEventQueueSize,
                                        &s_link.uart_events, 0));
    WakeSlave();
    ESP_ERROR_CHECK(uart_param_config(s_link.config.uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(s_link.config.uart_num, s_link.config.tx_pin,
                                 s_link.config.rx_pin, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    const rpc_701_config_t rpc_config = {.write_cb = UartWrite};
    if (rpc_701_init(&rpc_config) != SUCCESS) {
        ESP_LOGE(kTag, "JL701 RPC core initialization failed");
        return false;
    }
    rpc_resister_event_callbacks();

    if (xTaskCreate(RxTask, "jl701_rx", kRxTaskStackSize, nullptr,
                    kRxTaskPriority, &s_link.rx_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create the JL701 UART receive task");
        return false;
    }

    s_link.started = true;
    /* A failed first handshake is not fatal: the keep-alive task retries it. */
    InitializeSlave(false);

    if (xTaskCreate(HeartbeatTask, "jl701_hb", kHeartbeatTaskStackSize, nullptr,
                    kHeartbeatTaskPriority, &s_link.heartbeat_task) != pdPASS) {
        ESP_LOGE(kTag, "Failed to create the JL701 keep-alive task");
        return false;
    }

    ESP_LOGI(kTag, "JL701 audio link started");
    return true;
}

bool jl701_audio_link_ready(void) {
    return s_link.ready.load();
}

void jl701_audio_link_set_volume(uint8_t percent) {
    s_link.volume.store(percent > 100 ? 100 : percent);
    s_link.volume_dirty.store(true);
}

bool jl701_audio_link_write_pcm(const int16_t* pcm, size_t samples) {
    if (pcm == nullptr || !s_link.ready.load()) {
        return false;
    }

    /* Volume shares the transport with PCM, so it is applied here, on the task
     * that owns the audio stream, instead of from the caller that changed it. */
    if (s_link.volume_dirty.exchange(false) &&
        rpc_music_volume_set(s_link.volume.load()) != RPC_ERR_SUCCESS) {
        s_link.volume_dirty.store(true);
    }

    TickType_t last_wake = xTaskGetTickCount();
    for (size_t offset = 0; offset < samples; offset += kPcmPacketSamples) {
        const size_t count = std::min(kPcmPacketSamples, samples - offset);
        auto* packet = reinterpret_cast<uint8_t*>(
            const_cast<int16_t*>(pcm + offset));
        if (rpc_sen_audio_pcm(packet, count * sizeof(int16_t)) != RPC_ERR_SUCCESS) {
            return false;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kPcmPacketIntervalMs));
    }
    return true;
}
