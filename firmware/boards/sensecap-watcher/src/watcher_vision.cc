// SPDX-License-Identifier: Apache-2.0
#include "sensecap_watcher_config.h"
#include "watcher_vision_reply.h"
#include "sscma_client_io.h"
#include <pxa_board_common/vision_port.h>
#include <pxa_board_common/vision_jpeg_preview.h>
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
constexpr size_t kReceiveBytes = 1024;
// The Himax owns its sensor configuration. Zero dimensions explicitly mean
// its current native size, learned from each JPEG rather than assumed here.
constexpr pxa_camera_profile_t kProfiles[] = {
    {1, PXA_VISION_JPEG, 0, 0, 0, PXA_VISION_MAX_JPEG, 0,
        PXA_CAMERA_PROFILE_DEFAULT | PXA_CAMERA_PROFILE_NATIVE_SIZE},
};
struct Camera {
    sscma_client_io_t* io = nullptr;
    char* line = nullptr;
    uint8_t* receive = nullptr;
    size_t used = 0;
    uint16_t format = 0;
} camera;
pxa_status_t Prepare(void*, uint16_t format) {
    camera.format = format; camera.used = 0;
    camera.line = static_cast<char*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_TEMPORARY), kLineCapacity));
    if (!camera.line) return PXA_STATUS_RESOURCE_LIMIT;
    // SPI cannot DMA into the PSRAM line buffer on S3. Reserve a small
    // internal staging buffer before decoding starts, instead of allocating
    // a transient 4096-byte bounce buffer for every read under memory pressure.
    camera.receive = static_cast<uint8_t*>(pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_INTERNAL, PXA_MEMORY_TEMPORARY), kReceiveBytes));
    if (!camera.receive) return PXA_STATUS_RESOURCE_LIMIT;
    sscma_client_io_spi_config_t config{};
    config.cs_gpio_num = WATCHER_AI_SPI_CS; config.sync_gpio_num = -1;
    config.spi_mode = 0; config.wait_delay = 2; config.pclk_hz = 12 * 1000 * 1000;
    config.trans_queue_depth = 1;
    const auto status = sscma_client_new_io_spi_bus(reinterpret_cast<void*>(WATCHER_SD_HOST), &config, &camera.io);
    if (status != ESP_OK) return PXA_STATUS_IO_ERROR;
    // A previous session may have been interrupted while the Himax was
    // sending a frame. Let BREAK reach its command task before clearing the
    // transport FIFO, which otherwise can discard the stop command itself.
    constexpr char stop[] = "AT+BREAK\r\n";
    if (camera.io->flush(camera.io) != ESP_OK ||
        camera.io->write(camera.io, stop, sizeof(stop) - 1) != ESP_OK) return PXA_STATUS_IO_ERROR;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (camera.io->flush(camera.io) != ESP_OK) return PXA_STATUS_IO_ERROR;
    return PXA_STATUS_OK;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    if (!camera.io || !camera.line) return PXA_STATUS_BAD_STATE;
    camera.used = 0;
    const char* cmd = camera.format == PXA_VISION_JPEG ? "AT+SAMPLE=1\r\n" : "AT+INVOKE=1,0,1\r\n";
    if (camera.io->flush(camera.io) != ESP_OK || camera.io->write(camera.io, cmd, strlen(cmd)) != ESP_OK)
        return PXA_STATUS_IO_ERROR;
    const int64_t deadline = esp_timer_get_time() + INT64_C(12000000);
    size_t transferred = 0;
    unsigned lines = 0;
    while (esp_timer_get_time() < deadline) {
        size_t available = 0;
        if (camera.io->available(camera.io, &available) != ESP_OK) return PXA_STATUS_IO_ERROR;
        if (!available) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        const auto n = std::min({available, kReceiveBytes, kLineCapacity - camera.used - 1});
        if (!n) return PXA_STATUS_LIMIT_EXCEEDED;
        if (camera.io->read(camera.io, camera.receive, n) != ESP_OK) return PXA_STATUS_IO_ERROR;
        memcpy(camera.line + camera.used, camera.receive, n);
        transferred += n;
        camera.used += n; camera.line[camera.used] = 0;
        while (auto* newline = static_cast<char*>(memchr(camera.line, '\n', camera.used))) {
            ++lines;
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
    ESP_LOGW("watcher_vision", "Himax frame deadline expired: bytes=%u lines=%u pending=%u",
             unsigned(transferred), lines, unsigned(camera.used));
    return PXA_STATUS_TIMED_OUT;
}
void Finish(void*) {
    if (camera.io) {
        constexpr char stop[] = "AT+BREAK\r\n";
        (void)camera.io->write(camera.io, stop, sizeof(stop) - 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)camera.io->flush(camera.io);
        (void)camera.io->del(camera.io); camera.io = nullptr;
        gpio_set_level(WATCHER_AI_SPI_CS, 1);
        gpio_set_direction(WATCHER_AI_SPI_CS, GPIO_MODE_OUTPUT);
    }
    pxa_memory_release(camera.line); camera.line = nullptr; camera.used = 0;
    pxa_memory_release(camera.receive); camera.receive = nullptr;
}
}
bool WatcherInitializeVision() {
    // Logical capabilities are hardware-specific. Missing/invalid on-chip
    // models cause an explicit inference error; no model is installed here.
    const PxaBoardVisionDriver driver{nullptr, true, true, Prepare, Capture, Finish, kProfiles, 1,
        nullptr, nullptr, PxaBoardVisionPreviewJpeg};
    return pxa_board_vision_port_initialize(&driver);
}
