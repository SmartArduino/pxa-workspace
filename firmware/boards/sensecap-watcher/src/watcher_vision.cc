// SPDX-License-Identifier: Apache-2.0
#include "sensecap_watcher_config.h"
#include "watcher_vision_reply.h"
#include "sscma_client_io.h"
#include <pxa_board_common/vision_port.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kMetadataLimit = PXA_VISION_MAX_JSON;
constexpr size_t kLineCapacity = ((PXA_VISION_MAX_JPEG + 2) / 3) * 4 + kMetadataLimit + 1;
struct Camera {
    sscma_client_io_t* io = nullptr;
    char* line = nullptr;
    size_t used = 0;
    uint16_t format = 0;
} camera;
pxa_status_t Prepare(void*, uint16_t format) {
    camera.format = format; camera.used = 0;
    camera.line = static_cast<char*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY), kLineCapacity));
    if (!camera.line) return PXA_STATUS_RESOURCE_LIMIT;
    sscma_client_io_spi_config_t config{};
    config.cs_gpio_num = WATCHER_AI_SPI_CS; config.sync_gpio_num = -1;
    config.spi_mode = 0; config.wait_delay = 2; config.pclk_hz = 12 * 1000 * 1000;
    config.trans_queue_depth = 1;
    const auto status = sscma_client_new_io_spi_bus(reinterpret_cast<void*>(WATCHER_SD_HOST), &config, &camera.io);
    if (status != ESP_OK) return PXA_STATUS_IO_ERROR;
    return PXA_STATUS_OK;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    if (!camera.io || !camera.line) return PXA_STATUS_BAD_STATE;
    camera.used = 0;
    const char* cmd = camera.format == PXA_VISION_JPEG ? "AT+SAMPLE=1\r\n" : "AT+INVOKE=1,0,1\r\n";
    if (camera.io->flush(camera.io) != ESP_OK || camera.io->write(camera.io, cmd, strlen(cmd)) != ESP_OK)
        return PXA_STATUS_IO_ERROR;
    const int64_t deadline = esp_timer_get_time() + INT64_C(12000000);
    while (esp_timer_get_time() < deadline) {
        size_t available = 0;
        if (camera.io->available(camera.io, &available) != ESP_OK) return PXA_STATUS_IO_ERROR;
        if (!available) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        const auto n = std::min({available, size_t(4095), kLineCapacity - camera.used - 1});
        if (!n) return PXA_STATUS_LIMIT_EXCEEDED;
        if (camera.io->read(camera.io, camera.line + camera.used, n) != ESP_OK) return PXA_STATUS_IO_ERROR;
        camera.used += n; camera.line[camera.used] = 0;
        while (auto* newline = static_cast<char*>(memchr(camera.line, '\n', camera.used))) {
            const size_t consumed = size_t(newline - camera.line) + 1;
            size_t length = consumed - 1;
            if (length && camera.line[length - 1] == '\r') --length;
            camera.line[length] = 0;
            // Boot/log lines are not data frames. Do not publish chip IDs or
            // any unrelated AT reply as application inference output.
            size_t begin = 0;
            while (begin < length && (camera.line[begin] == '\r' || camera.line[begin] == ' ' || camera.line[begin] == '\t')) ++begin;
            if (begin < length && camera.line[begin] == '{') {
                const auto result = WatcherDecodeVisionReply(camera.line + begin, length - begin, camera.format, output, capacity, size);
                if (result != PXA_STATUS_WOULD_BLOCK) return result;
            }
            memmove(camera.line, camera.line + consumed, camera.used - consumed);
            camera.used -= consumed; camera.line[camera.used] = 0;
        }
    }
    ESP_LOGW("watcher_vision", "Himax frame deadline expired");
    return PXA_STATUS_TIMED_OUT;
}
void Finish(void*) {
    if (camera.io) {
        constexpr char stop[] = "AT+BREAK\r\n";
        (void)camera.io->write(camera.io, stop, sizeof(stop) - 1);
        (void)camera.io->flush(camera.io);
        (void)camera.io->del(camera.io); camera.io = nullptr;
        gpio_set_level(WATCHER_AI_SPI_CS, 1);
        gpio_set_direction(WATCHER_AI_SPI_CS, GPIO_MODE_OUTPUT);
    }
    pxa_memory_release(camera.line); camera.line = nullptr; camera.used = 0;
}
}
bool WatcherInitializeVision() {
    // Logical capabilities are hardware-specific. Missing/invalid on-chip
    // models cause an explicit inference error; no model is installed here.
    const PxaBoardVisionDriver driver{nullptr, true, true, Prepare, Capture, Finish};
    return pxa_board_vision_port_initialize(&driver);
}
