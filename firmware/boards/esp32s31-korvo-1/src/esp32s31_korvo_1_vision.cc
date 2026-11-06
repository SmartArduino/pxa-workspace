// SPDX-License-Identifier: Apache-2.0
#include <pxa_board_common/vision_port.h>
#include <pxa_board_common/vision_jpeg_preview.h>
#include "korvo_camera_buffers.h"
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
constexpr pxa_camera_profile_t kProfiles[] = {
    {1, PXA_VISION_JPEG, 320, 240, 0, PXA_VISION_MAX_JPEG, 40000, PXA_CAMERA_PROFILE_DEFAULT},
#if CONFIG_CAMERA_OV3660_DVP_JPEG_640X480_25FPS
    {2, PXA_VISION_JPEG, 640, 480, 0, PXA_VISION_MAX_JPEG, 40000, 0},
#endif
};
struct Camera {
    i2c_master_bus_handle_t bus = nullptr;
    esp_sccb_io_handle_t sccb = nullptr;
    esp_cam_sensor_device_t* sensor = nullptr;
    esp_cam_ctlr_handle_t controller = nullptr;
    void* allocation = nullptr;
    uint8_t* buffers[2]{};
    uint8_t* last_issued = nullptr;
    size_t stride = 0, received = 0;
    size_t buffer_bytes = 0;
    std::atomic<void*> completed{nullptr};
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done = nullptr;
    bool enabled = false, started = false;
} camera;
bool IRAM_ATTR GetBuffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    // The first completed buffer remains immutable. Further frames only use
    // the other buffer until the worker stops the controller and copies it.
    camera.last_issued = KorvoCameraNextBuffer(camera.buffers[0], camera.buffers[1],
        static_cast<const uint8_t*>(camera.completed.load(std::memory_order_acquire)), camera.last_issued);
    trans->buffer = camera.last_issued;
    trans->buflen = camera.stride;
    return false;
}
bool IRAM_ATTR Completed(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    // Sensor restart may produce an incomplete JPEG. Keep alternating until a
    // complete frame arrives, within Capture's existing three-second deadline.
    if (camera.completed.load(std::memory_order_relaxed) || trans->received_size < 4 ||
        trans->received_size > camera.buffer_bytes) return false;
    camera.received = trans->received_size;
    camera.completed.store(trans->buffer, std::memory_order_release);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(camera.done, &wake);
    return wake == pdTRUE;
}
esp_err_t ConfigureImage() {
    // The vendor QVGA/VGA JPEG tables finish with brightness +2, contrast +3
    // and the saturation -2 matrix. Apply neutral ISP controls after the table
    // so switching profiles cannot restore the washed-out image.
    const struct { uint32_t id; int value; } controls[] = {
        {ESP_CAM_SENSOR_WB, 0},
        {ESP_CAM_SENSOR_AE_LEVEL, -2}, // Preserve the vendor's automatic AE target.
        {ESP_CAM_SENSOR_CONTRAST, 0},
        {ESP_CAM_SENSOR_SATURATION, 0},
        {ESP_CAM_SENSOR_JPEG_QUALITY, 34}, // QS=7, versus the table's QS=10.
    };
    for (const auto& control : controls) {
        const auto error = esp_cam_sensor_set_para_value(camera.sensor, control.id,
            &control.value, sizeof(control.value));
        if (error != ESP_OK) return error;
    }
    // esp_cam_sensor's OV3660 driver has no brightness control. Use its public
    // register API for the neutral brightness magnitude; no per-frame filter.
    esp_cam_sensor_reg_val_t brightness{0x5587, 0};
    auto error = esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_S_REG, &brightness);
    if (error != ESP_OK) return error;
    for (const auto address : {0x3406u, 0x3503u, 0x5586u, 0x5587u, 0x4407u}) {
        esp_cam_sensor_reg_val_t reg{address, 0};
        error = esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_G_REG, &reg);
        if (error != ESP_OK) return error;
        ESP_LOGI(kTag, "ISP reg 0x%04x=0x%02x", unsigned(address), unsigned(reg.value));
    }
    ESP_LOGI(kTag, "ISP: auto WB, AE -2, brightness/contrast/saturation 0, JPEG QS 7");
    return ESP_OK;
}
pxa_status_t PrepareProfile(void*, const pxa_camera_profile_t& profile) {
    camera.buffer_bytes = size_t(profile.width) * profile.height * 2;
    esp_cam_ctlr_dvp_pin_config_t pins{};
    pins.data_width = static_cast<cam_ctlr_data_width_t>(8);
    for (unsigned i = 0; i < 8; ++i) pins.data_io[i] = static_cast<gpio_num_t>(46 + i);
    pins.pclk_io = GPIO_NUM_54; pins.xclk_io = GPIO_NUM_55;
    pins.vsync_io = GPIO_NUM_56; pins.de_io = GPIO_NUM_57;
    esp_cam_ctlr_dvp_config_t config{};
    config.ctlr_id = 0; config.clk_src = CAM_CLK_SRC_DEFAULT;
    config.h_res = profile.width; config.v_res = profile.height;
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
        if (f.format == ESP_CAM_SENSOR_PIXFORMAT_JPEG && f.width == profile.width && f.height == profile.height)
            selected = &f;
    }
    if (!selected) return PXA_STATUS_UNSUPPORTED;
    if (esp_cam_sensor_set_format(camera.sensor, selected) != ESP_OK) return PXA_STATUS_IO_ERROR;
    error = ConfigureImage();
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "ISP setup: %s", esp_err_to_name(error));
        return PXA_STATUS_IO_ERROR;
    }
    size_t alignment = 0;
    if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment) != ESP_OK || !alignment ||
        alignment > 4096 || (alignment & (alignment - 1))) return PXA_STATUS_INTERNAL;
    camera.stride = (camera.buffer_bytes + alignment - 1) & ~(alignment - 1);
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
    ESP_LOGI(kTag, "OV3660 JPEG ready: %ux%u, CAM clock 20 MHz", profile.width, profile.height);
    return PXA_STATUS_OK;
}
pxa_status_t Prepare(void* context, uint16_t format) {
    return format == PXA_VISION_JPEG ? PrepareProfile(context, kProfiles[0]) : PXA_STATUS_UNSUPPORTED;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    *size = 0;
    camera.completed.store(nullptr, std::memory_order_release);
    camera.last_issued = nullptr;
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
    if (!pixels || n < 4 || n > camera.buffer_bytes || n > capacity) return PXA_STATUS_PROTOCOL_ERROR;
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
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish,
        kProfiles, sizeof(kProfiles)/sizeof(kProfiles[0]), PrepareProfile, nullptr, PxaBoardVisionPreviewJpeg};
    return pxa_board_vision_port_initialize(&driver);
}
