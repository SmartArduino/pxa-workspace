// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/vision_port.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <driver/i2c_master.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_dvp.h>
#include <esp_cam_sensor.h>
// esp_sccb_intf 1.0.0's I2C declaration lacks C++ linkage guards.
extern "C" {
#include <esp_sccb_i2c.h>
}
#include <ov3660.h>
#include <esp_cache.h>
#include <esp_private/esp_cache_private.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <cstring>

namespace {
constexpr char kTag[] = "korvo_camera";
constexpr unsigned kWidth = 320, kHeight = 240;
constexpr size_t kBufferBytes = kWidth * kHeight * 2;
struct Camera {
    i2c_master_bus_handle_t bus = nullptr;
    esp_sccb_io_handle_t sccb = nullptr;
    esp_cam_sensor_device_t* sensor = nullptr;
    esp_cam_ctlr_handle_t controller = nullptr;
    void* allocation = nullptr;
    uint8_t* buffers[2]{};
    size_t stride = 0, received = 0;
    std::atomic<void*> completed{nullptr};
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = nullptr;
    bool enabled = false, started = false;
} camera;
bool IRAM_ATTR GetBuffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    // The first completed buffer remains immutable. Further frames only use
    // the other buffer until the worker stops the controller and copies it.
    trans->buffer = camera.completed.load(std::memory_order_acquire) == camera.buffers[0] ? camera.buffers[1] : camera.buffers[0];
    trans->buflen = camera.stride;
    return false;
}
bool IRAM_ATTR Completed(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    if (camera.completed.load(std::memory_order_relaxed)) return false;
    camera.received = trans->received_size;
    camera.completed.store(trans->buffer, std::memory_order_release);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(camera.done, &wake);
    return wake == pdTRUE;
}
pxa_status_t Prepare(void*, uint16_t format) {
    if (format != PXA_VISION_JPEG) return PXA_STATUS_UNSUPPORTED;
    esp_cam_ctlr_dvp_pin_config_t pins{};
    pins.data_width = static_cast<cam_ctlr_data_width_t>(8);
    for (unsigned i = 0; i < 8; ++i) pins.data_io[i] = static_cast<gpio_num_t>(46 + i);
    pins.pclk_io = GPIO_NUM_54; pins.xclk_io = GPIO_NUM_55;
    pins.vsync_io = GPIO_NUM_56; pins.de_io = GPIO_NUM_57;
    esp_cam_ctlr_dvp_config_t config{};
    config.ctlr_id = 0; config.clk_src = CAM_CLK_SRC_DEFAULT;
    config.h_res = kWidth; config.v_res = kHeight;
    config.input_data_color_type = CAM_CTLR_COLOR_RGB565;
    config.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    config.cam_data_width = 8; config.pic_format_jpeg = true;
    config.bk_buffer_dis = true; config.dma_burst_size = 64;
    config.xclk_freq = 20000000; config.pin = &pins;
    // Use the IDF CAM clock path before sensor detection. No LEDC clock or
    // irreversible silicon configuration is involved.
    auto error = esp_cam_new_dvp_ctlr(&config, &camera.controller);
    if (error != ESP_OK) { ESP_LOGW(kTag, "DVP setup: %s", esp_err_to_name(error)); return PXA_STATUS_IO_ERROR; }
    vTaskDelay(pdMS_TO_TICKS(20));
    sccb_i2c_config_t sccb{};
    sccb.scl_speed_hz = 100000; sccb.device_address = OV3660_SCCB_ADDR;
    sccb.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    error = sccb_new_i2c_io(camera.bus, &sccb, &camera.sccb);
    if (error != ESP_OK) return PXA_STATUS_IO_ERROR;
    esp_cam_sensor_config_t sensor{};
    sensor.sccb_handle = camera.sccb; sensor.xclk_pin = GPIO_NUM_NC;
    sensor.reset_pin = GPIO_NUM_NC; sensor.pwdn_pin = GPIO_NUM_NC;
    sensor.xclk_freq_hz = 20000000; sensor.sensor_port = ESP_CAM_SENSOR_DVP;
    camera.sensor = ov3660_detect(&sensor);
    if (!camera.sensor) {
        ESP_LOGW(kTag, "OV3660 not detected with CAM XCLK on GPIO55");
        return PXA_STATUS_UNAVAILABLE;
    }
    esp_cam_sensor_format_array_t formats{};
    if (esp_cam_sensor_query_format(camera.sensor, &formats) != ESP_OK) return PXA_STATUS_IO_ERROR;
    const esp_cam_sensor_format_t* selected = nullptr;
    for (unsigned i = 0; i < formats.count; ++i) {
        const auto& f = formats.format_array[i];
        if (f.format == ESP_CAM_SENSOR_PIXFORMAT_JPEG && f.width == kWidth && f.height == kHeight)
            selected = &f;
    }
    if (!selected) return PXA_STATUS_UNSUPPORTED;
    if (esp_cam_sensor_set_format(camera.sensor, selected) != ESP_OK) return PXA_STATUS_IO_ERROR;
    size_t alignment = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment) != ESP_OK || !alignment ||
        alignment > 4096 || (alignment & (alignment - 1))) return PXA_STATUS_INTERNAL;
    camera.stride = (kBufferBytes + alignment - 1) & ~(alignment - 1);
    camera.allocation = pxa_memory_allocate(
        pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_FRAME), 2 * camera.stride + alignment - 1);
    if (!camera.allocation) return PXA_STATUS_RESOURCE_LIMIT;
    const auto aligned = (reinterpret_cast<uintptr_t>(camera.allocation) + alignment - 1) & ~(uintptr_t(alignment) - 1);
    camera.buffers[0] = reinterpret_cast<uint8_t*>(aligned);
    camera.buffers[1] = camera.buffers[0] + camera.stride;
    const esp_cam_ctlr_evt_cbs_t callbacks{GetBuffer, Completed, nullptr};
    if (esp_cam_ctlr_register_event_callbacks(camera.controller, &callbacks, nullptr) != ESP_OK) return PXA_STATUS_IO_ERROR;
    int stream = 1;
    if (esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream) != ESP_OK) return PXA_STATUS_IO_ERROR;
    ESP_LOGI(kTag, "OV3660 JPEG ready: %ux%u, CAM clock 20 MHz", kWidth, kHeight);
    return PXA_STATUS_OK;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    camera.completed.store(nullptr, std::memory_order_release);
    camera.received = 0;
    while (xSemaphoreTake(camera.done, 0) == pdTRUE) {}
    if (esp_cam_ctlr_enable(camera.controller) != ESP_OK) return PXA_STATUS_IO_ERROR;
    camera.enabled = true;
    if (esp_cam_ctlr_start(camera.controller) != ESP_OK) return PXA_STATUS_IO_ERROR;
    camera.started = true;
    const bool received = xSemaphoreTake(camera.done, pdMS_TO_TICKS(3000)) == pdTRUE;
    if (esp_cam_ctlr_stop(camera.controller) != ESP_OK) return PXA_STATUS_IO_ERROR;
    camera.started = false;
    if (esp_cam_ctlr_disable(camera.controller) != ESP_OK) return PXA_STATUS_IO_ERROR;
    camera.enabled = false;
    if (!received) return PXA_STATUS_TIMED_OUT;
    auto* pixels = static_cast<uint8_t*>(camera.completed.load(std::memory_order_acquire));
    const auto n = camera.received;
    if (!pixels || n < 4 || n > kBufferBytes || n > capacity) return PXA_STATUS_PROTOCOL_ERROR;
    if (esp_cache_msync(pixels, camera.stride, ESP_CACHE_MSYNC_FLAG_DIR_M2C) != ESP_OK) return PXA_STATUS_IO_ERROR;
    if (pixels[0] != 0xff || pixels[1] != 0xd8 || pixels[n - 2] != 0xff || pixels[n - 1] != 0xd9)
        return PXA_STATUS_PROTOCOL_ERROR;
    memcpy(output, pixels, n); *size = n;
    return PXA_STATUS_OK;
}
void Finish(void*) {
    if (camera.started) { (void)esp_cam_ctlr_stop(camera.controller); camera.started = false; }
    if (camera.enabled) { (void)esp_cam_ctlr_disable(camera.controller); camera.enabled = false; }
    if (camera.sensor) {
        int stream = 0;
        (void)esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream);
        (void)esp_cam_sensor_del_dev(camera.sensor); camera.sensor = nullptr;
    }
    if (camera.sccb) { (void)esp_sccb_del_i2c_io(camera.sccb); camera.sccb = nullptr; }
    if (camera.controller) { (void)esp_cam_ctlr_del(camera.controller); camera.controller = nullptr; }
    pxa_memory_release(camera.allocation); camera.allocation = nullptr;
    camera.buffers[0] = camera.buffers[1] = nullptr;
    camera.completed.store(nullptr, std::memory_order_release);
}
}
bool Esp32S31Korvo1InitializeVision(i2c_master_bus_handle_t bus) {
    if (!bus || camera.done) return false;
    camera.bus = bus;
    camera.done = xSemaphoreCreateBinaryStatic(&camera.done_storage);
    if (!camera.done) return false;
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish};
    return pxa_board_vision_port_initialize(&driver);
}
