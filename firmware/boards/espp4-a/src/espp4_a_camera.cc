// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_camera.h"
#include "espp4_a_camera_buffers.h"
#include "espp4_a_camera_geometry.h"
#include <pxa_board_common/vision_port.h>
#include <pxa_board_common/vision_jpeg_preview.h>
#include <pxa/pxa_esp_resource_memory.h>
#include <driver/gpio.h>
#include <driver/ppa.h>
#include <driver/i2c_master.h>
#include <driver/isp.h>
#include <driver/jpeg_encode.h>
#include <esp_cam_ctlr.h>
#include <esp_cam_ctlr_csi.h>
#include <esp_cam_sensor.h>
#include <esp_cam_sensor_xclk.h>
extern "C" {
#include <esp_sccb_i2c.h>
}
#include <gc4023.h>
#include <esp_cache.h>
#include <esp_private/esp_cache_private.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <soc/mipi_csi_host_struct.h>
#include <soc/isp_struct.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>

namespace {
constexpr char kTag[] = "espp4_camera";
constexpr unsigned kSensorWidth = 1920, kSensorHeight = 1080;
constexpr size_t kSensorBytes = kSensorWidth * kSensorHeight * 2;
constexpr pxa_camera_profile_t kProfiles[] = {
    {9, PXA_VISION_JPEG, 320, 240, 75, PXA_VISION_MAX_JPEG, 40000, 0},
    {1, PXA_VISION_JPEG, 320, 180, 75, PXA_VISION_MAX_JPEG, 40000, 0},
    {2, PXA_VISION_JPEG, 640, 360, 80, PXA_VISION_MAX_JPEG, 40000, 0},
    {3, PXA_VISION_JPEG, 160, 90, 60, PXA_VISION_MAX_JPEG, 40000, 0},
    {4, PXA_VISION_JPEG, 480, 270, 78, PXA_VISION_MAX_JPEG, 40000, 0},
    {5, PXA_VISION_JPEG, 800, 450, 80, PXA_VISION_MAX_JPEG, 40000, 0},
    {6, PXA_VISION_JPEG, 960, 540, 78, PXA_VISION_MAX_JPEG, 40000, 0},
    {7, PXA_VISION_JPEG, 1280, 720, 70, PXA_VISION_MAX_JPEG, 40000, 0},
    {8, PXA_VISION_JPEG, 1920, 1080, 60, PXA_VISION_MAX_JPEG, 40000,
        PXA_CAMERA_PROFILE_NATIVE_SIZE | PXA_CAMERA_PROFILE_DEFAULT},
    {10, PXA_VISION_JPEG, 1920, 1080, 82, PXA_VISION_MAX_JPEG_HD, 40000,
        PXA_CAMERA_PROFILE_NATIVE_SIZE},
    {11, PXA_VISION_JPEG, 240, 320, 75, PXA_VISION_MAX_JPEG, 40000, 0},
    {12, PXA_VISION_JPEG, 180, 320, 75, PXA_VISION_MAX_JPEG, 40000, 0},
    {13, PXA_VISION_JPEG, 270, 480, 78, PXA_VISION_MAX_JPEG, 40000, 0},
    {14, PXA_VISION_JPEG, 720, 1280, 70, PXA_VISION_MAX_JPEG, 40000, 0},
    {15, PXA_VISION_JPEG, 1080, 1920, 60, PXA_VISION_MAX_JPEG, 40000, PXA_CAMERA_PROFILE_NATIVE_SIZE},
    {16, PXA_VISION_JPEG, 1080, 1920, 82, PXA_VISION_MAX_JPEG_HD, 40000, PXA_CAMERA_PROFILE_NATIVE_SIZE},
};
struct Camera {
    i2c_master_bus_handle_t bus = nullptr;
    esp_sccb_io_handle_t sccb = nullptr;
    esp_cam_sensor_device_t* sensor = nullptr;
    esp_cam_sensor_xclk_handle_t xclk = nullptr;
    esp_cam_ctlr_handle_t controller = nullptr;
    isp_proc_handle_t isp = nullptr;
    jpeg_encoder_handle_t encoder = nullptr;
    ppa_client_handle_t ppa = nullptr;
    uint8_t* preview_dma = nullptr;
    void* allocation = nullptr;
    uint8_t* buffers[2]{};
    uint8_t* scaled = nullptr;
    size_t jpeg_capacity = 0;
    uint8_t* last = nullptr;
    uint16_t* columns = nullptr;
    pxa_raster_asset_t* pending_preview = nullptr;
    std::atomic<void*> completed{nullptr};
    std::atomic<uint32_t> requests{0}, completions{0}, errors{0};
    std::atomic<uint32_t> ready_us{0};
    StaticSemaphore_t done_storage{};
    SemaphoreHandle_t done = nullptr;
    size_t stride = 0, scaled_stride = 0;
    unsigned width = 0, height = 0, quality = 0, frames = 0;
    unsigned crop_x = 0, crop_width = kSensorWidth;
    uint32_t exposure = 1000, gain = 0;
    bool enabled = false, started = false, isp_enabled = false;
    bool portrait = false, ppa_failed = false;
    bool demosaic = false, gamma = false, wbg = false, blc = false;
    uint32_t red_gain = 256, blue_gain = 256;
    uint64_t wait_us = 0, sample_us = 0, encode_us = 0;
    unsigned captures = 0, previews = 0;
    unsigned stale_drops = 0;
};
// CSI callbacks and all state touched by them remain accessible during Flash writes.
DRAM_ATTR Camera camera;
bool IRAM_ATTR GetBuffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    camera.requests.fetch_add(1, std::memory_order_relaxed);
    auto* held = camera.completed.load(std::memory_order_acquire);
    camera.last = static_cast<uint8_t*>(espp4_a::CameraNextBuffer(
        camera.buffers[0], camera.buffers[1], held, camera.last));
    trans->buffer = camera.last;
    trans->buflen = camera.stride;
    return false;
}
bool IRAM_ATTR Completed(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* trans, void*) {
    camera.completions.fetch_add(1, std::memory_order_relaxed);
    // The CSI driver selects/restarts the next transfer before this callback.
    // If it reused the current DMA buffer, that frame cannot be handed to the worker.
    if (!espp4_a::CameraCanPublish(trans->buffer, camera.last,
            camera.completed.load(std::memory_order_relaxed), trans->received_size, kSensorBytes))
        return false;
    camera.ready_us.store(uint32_t(esp_timer_get_time()), std::memory_order_relaxed);
    camera.completed.store(trans->buffer, std::memory_order_release);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(camera.done, &wake);
    return wake == pdTRUE;
}
bool IRAM_ATTR CsiError(esp_cam_ctlr_handle_t, const esp_cam_ctlr_error_event_data_t* event, void*) {
    camera.errors.fetch_or(event->csi_host_err_evts, std::memory_order_relaxed);
    return false;
}
pxa_status_t Error(const char* operation, esp_err_t error) {
    ESP_LOGW(kTag, "%s: %s", operation, esp_err_to_name(error));
    return error == ESP_ERR_NO_MEM ? PXA_STATUS_RESOURCE_LIMIT : PXA_STATUS_IO_ERROR;
}
void Finish(void*) {
    if (camera.started) (void)esp_cam_ctlr_stop(camera.controller);
    if (camera.enabled) (void)esp_cam_ctlr_disable(camera.controller);
    camera.started = camera.enabled = false;
    if (camera.sensor) {
        int stream = 0;
        (void)esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream);
        (void)esp_cam_sensor_del_dev(camera.sensor); camera.sensor = nullptr;
    }
    if (camera.controller) { (void)esp_cam_ctlr_del(camera.controller); camera.controller = nullptr; }
    if (camera.wbg) (void)esp_isp_wbg_disable(camera.isp);
    camera.wbg = false;
    if (camera.blc) (void)esp_isp_blc_disable(camera.isp);
    camera.blc = false;
    if (camera.demosaic) (void)esp_isp_demosaic_disable(camera.isp);
    if (camera.gamma) (void)esp_isp_gamma_disable(camera.isp);
    camera.demosaic = camera.gamma = false;
    if (camera.isp_enabled) (void)esp_isp_disable(camera.isp);
    camera.isp_enabled = false;
    if (camera.isp) { (void)esp_isp_del_processor(camera.isp); camera.isp = nullptr; }
    if (camera.encoder) { (void)jpeg_del_encoder_engine(camera.encoder); camera.encoder = nullptr; }
    if (camera.ppa) { (void)ppa_unregister_client(camera.ppa); camera.ppa = nullptr; }
    camera.preview_dma = nullptr;
    if (camera.sccb) { (void)esp_sccb_del_i2c_io(camera.sccb); camera.sccb = nullptr; }
    if (camera.bus) { (void)i2c_del_master_bus(camera.bus); camera.bus = nullptr; }
    if (camera.xclk) {
        (void)esp_cam_sensor_xclk_stop(camera.xclk);
        (void)esp_cam_sensor_xclk_free(camera.xclk); camera.xclk = nullptr;
    }
    heap_caps_free(camera.allocation); camera.allocation = nullptr;
    camera.buffers[0] = camera.buffers[1] = camera.scaled = camera.last = nullptr;
    camera.completed.store(nullptr);
    camera.columns = nullptr;
    pxa_raster_asset_release(camera.pending_preview); camera.pending_preview = nullptr;
    if (camera.done) while (xSemaphoreTake(camera.done, 0) == pdTRUE) {}
}
pxa_status_t PrepareProfile(void*, const pxa_camera_profile_t& profile) {
    Finish(nullptr);
    if (profile.id < 1 || profile.id > sizeof(kProfiles) / sizeof(kProfiles[0])) return PXA_STATUS_UNSUPPORTED;
    camera.width = profile.width; camera.height = profile.height; camera.quality = profile.quality;
    camera.portrait = espp4_a::CameraPortrait(camera.width, camera.height);
    camera.ppa_failed = false;
    camera.jpeg_capacity = profile.max_frame_bytes;
    camera.crop_width = std::min(kSensorWidth, espp4_a::CameraCropWidth(camera.width, camera.height));
    camera.crop_x = (kSensorWidth - camera.crop_width) / 2;
    camera.frames = 0; camera.exposure = 1000; camera.gain = 0;
    camera.requests = 0; camera.completions = 0; camera.errors = 0;
    camera.wait_us = camera.sample_us = camera.encode_us = 0;
    camera.captures = camera.previews = 0;
    camera.stale_drops = 0;
    if (!camera.done) camera.done = xSemaphoreCreateBinaryStatic(&camera.done_storage);
    auto error = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &camera.xclk);
    if (error != ESP_OK) return Error("MCLK allocate", error);
    esp_cam_sensor_xclk_config_t clock{};
    clock.esp_clock_router_cfg.xclk_pin = GPIO_NUM_21;
    clock.esp_clock_router_cfg.xclk_freq_hz = 24000000;
    if ((error = esp_cam_sensor_xclk_start(camera.xclk, &clock)) != ESP_OK) return Error("MCLK start", error);
    // Board manager holds CSI_RSTN low before detection in the supplier demo.
    // Reproduce that sequencing for cold boot and every permission session.
    const gpio_config_t reset{.pin_bit_mask = 1ULL << 11, .mode = GPIO_MODE_OUTPUT};
    if ((error = gpio_set_level(GPIO_NUM_11, 0)) != ESP_OK ||
        (error = gpio_config(&reset)) != ESP_OK) return Error("sensor reset", error);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2c_master_bus_config_t bus{};
    bus.i2c_port = LP_I2C_NUM_0; bus.sda_io_num = GPIO_NUM_9; bus.scl_io_num = GPIO_NUM_10;
    bus.lp_source_clk = LP_I2C_SCLK_DEFAULT; bus.flags.enable_internal_pullup = true;
    if ((error = i2c_new_master_bus(&bus, &camera.bus)) != ESP_OK) return Error("LP I2C", error);
    sccb_i2c_config_t sccb{};
    sccb.scl_speed_hz = 100000; sccb.device_address = GC4023_SCCB_ADDR;
    sccb.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    if ((error = sccb_new_i2c_io(camera.bus, &sccb, &camera.sccb)) != ESP_OK) return Error("SCCB", error);
    esp_cam_sensor_config_t sensor{};
    sensor.sccb_handle = camera.sccb; sensor.xclk_pin = GPIO_NUM_NC;
    sensor.reset_pin = GPIO_NUM_11; sensor.pwdn_pin = GPIO_NUM_NC;
    sensor.xclk_freq_hz = 24000000; sensor.sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    camera.sensor = gc4023_detect(&sensor);
    if (!camera.sensor) return PXA_STATUS_UNAVAILABLE;
    if ((error = esp_cam_sensor_set_format(camera.sensor, nullptr)) != ESP_OK) return Error("sensor format", error);
    size_t alignment = 0;
    if ((error = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment)) != ESP_OK || !alignment)
        return PXA_STATUS_INTERNAL;
    const auto align = [alignment](size_t n) { return (n + alignment - 1) & ~(alignment - 1); };
    camera.stride = align(kSensorBytes);
    const bool native = espp4_a::CameraNativePixels(camera.width, camera.height);
    const bool direct_native = native && !camera.portrait;
    const bool shared_preview = (camera.portrait ? camera.height : camera.width) == espp4_a::kCameraPreviewWidth;
    camera.scaled_stride = direct_native || shared_preview ? 0 : align(size_t(camera.width) * camera.height * 2);
    if (native) {
        ppa_client_config_t ppa_config{};
        ppa_config.oper_type = PPA_OPERATION_SRM;
        const auto registered = ppa_register_client(&ppa_config, &camera.ppa);
        if (registered != ESP_OK) ESP_LOGW(kTag, "PPA preview unavailable: %s", esp_err_to_name(registered));
    }
    const size_t preview_bytes = camera.ppa ? align(espp4_a::kCameraPpaPreviewBytes) : 0;
    const size_t bytes = 2 * camera.stride + camera.scaled_stride + preview_bytes + align(std::max(camera.width, camera.height) * 2);
    // Fixed board-owned CSI/JPEG DMA storage, separate from immutable Core assets.
    // The allocation is bounded by the two fixed sensor frames and selected profile.
    camera.allocation = heap_caps_aligned_alloc(alignment, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!camera.allocation) return PXA_STATUS_RESOURCE_LIMIT;
    const auto address = (reinterpret_cast<uintptr_t>(camera.allocation) + alignment - 1) & ~(uintptr_t(alignment) - 1);
    camera.buffers[0] = reinterpret_cast<uint8_t*>(address);
    camera.buffers[1] = camera.buffers[0] + camera.stride;
    camera.scaled = camera.buffers[1] + camera.stride;
    camera.preview_dma = camera.ppa ? camera.scaled + camera.scaled_stride : nullptr;
    camera.columns = reinterpret_cast<uint16_t*>(camera.scaled + camera.scaled_stride + preview_bytes);
    espp4_a::CameraColumns(camera.columns, camera.crop_width, camera.portrait ? camera.height : camera.width);
    // LDO3 is already held by the DSI panel. Never release/reconfigure that shared rail.
    esp_cam_ctlr_csi_config_t csi{};
    csi.ctlr_id = 0; csi.h_res = kSensorWidth; csi.v_res = kSensorHeight;
    csi.data_lane_num = 2; csi.lane_bit_rate_mbps = 720;
    // With ISP in the CSI path, the bridge receives the ISP's RGB565 output.
    // RAW10 belongs to the ISP input, not the CSI bridge format converter.
    csi.input_data_color_type = CAM_CTLR_COLOR_RGB565; csi.output_data_color_type = CAM_CTLR_COLOR_RGB565;
    csi.queue_items = 1; csi.bk_buffer_dis = true;
    if ((error = esp_cam_new_csi_ctlr(&csi, &camera.controller)) != ESP_OK) return Error("CSI create", error);
    const esp_cam_ctlr_evt_cbs_t callbacks{GetBuffer, Completed, CsiError};
    if ((error = esp_cam_ctlr_register_event_callbacks(camera.controller, &callbacks, nullptr)) != ESP_OK) return Error("CSI callbacks", error);
    isp_proc_handle_t& isp = camera.isp;
    esp_isp_processor_cfg_t config{};
    // Two 720 Mbps RAW10 lanes deliver bursts at up to 144 Mpixel/s.
    config.clk_src = ISP_CLK_SRC_PLL160;
    config.clk_hz = 160000000; config.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    config.input_data_color_type = ISP_COLOR_RAW10; config.output_data_color_type = ISP_COLOR_RGB565;
    config.h_res = kSensorWidth; config.v_res = kSensorHeight; config.bayer_order = COLOR_RAW_ELEMENT_ORDER_RGGB;
    if ((error = esp_isp_new_processor(&config, &isp)) != ESP_OK) return Error("ISP create", error);
    if ((error = esp_isp_enable(isp)) != ESP_OK) return Error("ISP enable", error);
    camera.isp_enabled = true;
    // The supplier's GC4023 tuning uses the official GC4053 ISP baseline:
    // black offset 16 in the ISP's normalized channel domain.
    esp_isp_blc_config_t black{};
    black.window.btm_right = {kSensorWidth, kSensorHeight};
    black.stretch = {true, true, true, true};
    black.flags.update_once_configured = true;
    if ((error = esp_isp_blc_configure(isp, &black)) != ESP_OK) return Error("black level configure", error);
    if ((error = esp_isp_blc_enable(isp)) != ESP_OK) return Error("black level enable", error);
    camera.blc = true;
    esp_isp_blc_offset_t offset{16, 16, 16, 16};
    if ((error = esp_isp_blc_set_correction_offset(isp, &offset)) != ESP_OK) return Error("black level offset", error);
    esp_isp_demosaic_config_t demosaic{};
    demosaic.grad_ratio.integer = 1;
    demosaic.padding_mode = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA;
    if ((error = esp_isp_demosaic_configure(isp, &demosaic)) != ESP_OK) return Error("demosaic configure", error);
    if ((error = esp_isp_demosaic_enable(isp)) != ESP_OK) return Error("demosaic enable", error);
    camera.demosaic = true;
    isp_gamma_curve_points_t curve{};
    if ((error = esp_isp_gamma_fill_curve_points([](uint32_t x) -> uint32_t {
        return x == 256 ? 256 : std::min<uint32_t>(255, uint32_t(std::pow(float(x) / 256.f, 1.f / 2.2f) * 256.f));
    }, &curve)) != ESP_OK) return Error("gamma curve", error);
    for (auto component : {COLOR_COMPONENT_R, COLOR_COMPONENT_G, COLOR_COMPONENT_B})
        if ((error = esp_isp_gamma_configure(isp, component, &curve)) != ESP_OK) return Error("gamma configure", error);
    if ((error = esp_isp_gamma_enable(isp)) != ESP_OK) return Error("gamma enable", error);
    camera.gamma = true;
    esp_isp_wbg_config_t white_balance{};
    // No sensor frames exist yet: apply initialization immediately so the
    // following gain write cannot collide with a pending frame-boundary update.
    white_balance.flags.update_once_configured = true;
    if ((error = esp_isp_wbg_configure(isp, &white_balance)) != ESP_OK) return Error("white balance configure", error);
    if ((error = esp_isp_wbg_enable(isp)) != ESP_OK) return Error("white balance enable", error);
    camera.wbg = true; camera.red_gain = camera.blue_gain = 256;
    if ((error = esp_isp_wbg_set_wb_gain(isp, {256, 256, 256})) != ESP_OK) return Error("white balance gain", error);
    jpeg_encode_engine_cfg_t jpeg{}; jpeg.timeout_ms = 500;
    if ((error = jpeg_new_encoder_engine(&jpeg, &camera.encoder)) != ESP_OK) return Error("JPEG create", error);
    if ((error = esp_cam_ctlr_enable(camera.controller)) != ESP_OK) return Error("CSI enable", error);
    camera.enabled = true;
    if (camera.ppa && (error = esp_cache_msync(camera.buffers[0], 2 * camera.stride,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)) != ESP_OK)
        return Error("CSI cache prepare", error);
    if ((error = esp_cam_ctlr_start(camera.controller)) != ESP_OK) return Error("CSI start", error);
    camera.started = true;
    int stream = 1;
    if ((error = esp_cam_sensor_ioctl(camera.sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream)) != ESP_OK) return Error("sensor stream", error);
    ESP_LOGI(kTag, "GC4023 CSI RAW10 1920x1080 25 fps -> RGB565/HW JPEG %ux%u q%u, buffers=%u", camera.width, camera.height, camera.quality, unsigned(bytes));
    return PXA_STATUS_OK;
}
pxa_status_t Prepare(void* context, uint16_t format) {
    return format == PXA_VISION_JPEG ? PrepareProfile(context, kProfiles[0]) : PXA_STATUS_UNSUPPORTED;
}
// Bounded host-side exposure and gray-world white balance. Sample the scaled
// frame at low cadence; sensor register writes remain on the permission worker.
void UpdateControls(const uint16_t* pixels, unsigned width, unsigned height) {
    if (camera.frames % 4) return;
    uint32_t red = 0, green = 0, blue = 0, count = 0;
    for (unsigned y = 4; y < height; y += 8) for (unsigned x = 4; x < width; x += 8) {
        const uint16_t p = pixels[y * width + x];
        red += ((p >> 11) & 31) * 255 / 31;
        green += ((p >> 5) & 63) * 255 / 63;
        blue += (p & 31) * 255 / 31; ++count;
    }
    const uint32_t luma = (77 * red + 150 * green + 29 * blue) / (256 * count);
    if (luma < 90 || luma > 120) {
        const uint32_t wanted = camera.exposure * 105 / std::max<uint32_t>(1, luma);
        const uint32_t exposure = std::clamp<uint32_t>(std::clamp<uint32_t>(wanted,
            camera.exposure * 3 / 4, camera.exposure * 5 / 4), 4u, 1642u);
        if (esp_cam_sensor_set_para_value(camera.sensor, ESP_CAM_SENSOR_EXPOSURE_VAL, &exposure, sizeof(exposure)) == ESP_OK)
            camera.exposure = exposure;
        uint32_t gain = camera.gain;
        if (exposure == 1642 && luma < 70) gain = std::min<uint32_t>(12, gain + 1);
        else if (luma > 150 && gain) --gain;
        if (gain != camera.gain && esp_cam_sensor_set_para_value(camera.sensor, ESP_CAM_SENSOR_GAIN, &gain, sizeof(gain)) == ESP_OK)
            camera.gain = gain;
    }
    if (red > count * 8 && green > count * 8 && blue > count * 8 && luma < 220) {
        const auto r = std::clamp<uint32_t>(camera.red_gain * green / red, 128u, 768u);
        const auto b = std::clamp<uint32_t>(camera.blue_gain * green / blue, 128u, 768u);
        const auto nr = (camera.red_gain * 7 + r) / 8, nb = (camera.blue_gain * 7 + b) / 8;
        if (esp_isp_wbg_set_wb_gain(camera.isp, {nr, 256, nb}) == ESP_OK) {
            camera.red_gain = nr; camera.blue_gain = nb;
        }
    }
}
pxa_status_t AcquireFrame(const uint16_t** output) {
    const int64_t begin = esp_timer_get_time();
    *output = nullptr;
    for (;;) {
        const int64_t remaining = 2000000 - (esp_timer_get_time() - begin);
        if (remaining <= 0 || (!camera.completed.load(std::memory_order_acquire) &&
            xSemaphoreTake(camera.done, std::max<TickType_t>(1, pdMS_TO_TICKS((remaining + 999) / 1000))) != pdTRUE)) {
            ESP_LOGW(kTag, "capture timeout: requests=%lu completed=%lu errors=0x%lx",
                static_cast<unsigned long>(camera.requests.load()), static_cast<unsigned long>(camera.completions.load()),
                static_cast<unsigned long>(camera.errors.load()));
            return PXA_STATUS_TIMED_OUT;
        }
        *output = static_cast<const uint16_t*>(camera.completed.load(std::memory_order_acquire));
        while (xSemaphoreTake(camera.done, 0) == pdTRUE) {}
        if (!*output) continue;
        if (espp4_a::CameraFrameFresh(camera.ready_us.load(std::memory_order_relaxed), uint32_t(esp_timer_get_time()))) break;
        camera.completed.store(nullptr, std::memory_order_release); ++camera.stale_drops;
    }
    // Native 1080p stays DMA-to-DMA: PPA and JPEG read CSI storage without
    // a CPU walk/invalidation of its full 4 MiB image.
    const auto error = camera.ppa && !camera.ppa_failed ? ESP_OK : esp_cache_msync(
        const_cast<uint16_t*>(*output), camera.stride, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (error != ESP_OK) {
        camera.completed.store(nullptr, std::memory_order_release);
        return Error("capture cache", error);
    }
    camera.wait_us += esp_timer_get_time() - begin;
    return PXA_STATUS_OK;
}
void* AllocateImage(void*, size_t bytes) {
    return pxa_memory_allocate(pxa_esp_device_resource_allocator(PXA_MEMORY_EXTERNAL, PXA_MEMORY_FRAME), bytes);
}
void ReleaseImage(void*, void* pointer) { pxa_memory_release(pointer); }
bool PpaImage(const uint16_t* source, void* output, size_t capacity, unsigned width, unsigned height) {
    if (!camera.ppa || camera.ppa_failed) return false;
    ppa_srm_oper_config_t scale{};
    scale.in.buffer = source; scale.in.pic_w = scale.in.block_w = kSensorWidth;
    scale.in.pic_h = scale.in.block_h = kSensorHeight;
    scale.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    scale.out.buffer = output; scale.out.buffer_size = capacity;
    scale.out.pic_w = width; scale.out.pic_h = height;
    scale.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    scale.rotation_angle = camera.portrait ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_0;
    scale.scale_x = float(camera.portrait ? height : width) / kSensorWidth;
    // Both axes use the same exact 1/16 scale; the shorter side rounds down.
    scale.scale_y = scale.scale_x;
    scale.mode = PPA_TRANS_MODE_BLOCKING;
    const auto error = ppa_do_scale_rotate_mirror(camera.ppa, &scale);
    if (error == ESP_OK) return true;
    camera.ppa_failed = true;
    ESP_LOGW(kTag, "PPA image unavailable: %s; using CPU transform", esp_err_to_name(error));
    return false;
}
pxa_status_t MakePreview(const uint16_t* source, pxa_raster_asset_t** output) {
    const int64_t begin = esp_timer_get_time();
    const bool use_ppa = camera.ppa && !camera.ppa_failed;
    const unsigned width = use_ppa ? (camera.portrait ? espp4_a::kCameraPpaPreviewHeight : espp4_a::kCameraPpaPreviewWidth) :
        (camera.portrait ? espp4_a::kCameraPreviewWidth * camera.width / camera.height : espp4_a::kCameraPreviewWidth);
    const unsigned height = use_ppa ? (camera.portrait ? espp4_a::kCameraPpaPreviewWidth : espp4_a::kCameraPpaPreviewHeight) :
        (camera.portrait ? espp4_a::kCameraPreviewWidth : camera.height * width / camera.width);
    pxa_asset_info_t info{};
    info.kind = PXA_ASSET_IMAGE; info.encoding = PXA_ASSET_ENCODING_RGB565; info.format_version = 1;
    info.payload_offset = PXA_ASSET_FILE_HEADER_BYTES; info.width = width; info.height = height;
    info.decoded_bytes = width * height * 2; info.stored_bytes = info.decoded_bytes + PXA_ASSET_FILE_HEADER_BYTES;
    uint8_t* bytes = nullptr;
    const auto result = pxa_asset_object_create(&info, AllocateImage, ReleaseImage, nullptr, output, &bytes);
    if (result != PXA_STATUS_OK) return result;
    auto* pixels = reinterpret_cast<uint16_t*>(bytes);
    // Native profiles use a fixed 7/16-size PPA preview; CPU fallback
    // samples CSI directly. Neither creates a full-resolution CPU copy.
    if (PpaImage(source, camera.preview_dma, espp4_a::kCameraPpaPreviewBytes, width, height)) {
        const auto error = esp_cache_msync(camera.preview_dma,
            espp4_a::kCameraPpaPreviewBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (error != ESP_OK) {
            pxa_raster_asset_release(*output); *output = nullptr;
            return Error("PPA preview", error);
        }
        // Only the small preview crosses into immutable Core image storage.
        std::memcpy(pixels, camera.preview_dma, width * height * 2);
    } else {
        if (use_ppa) { // A failed DMA attempt may leave the full CSI image uncached.
            const auto error = esp_cache_msync(const_cast<uint16_t*>(source), camera.stride, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            if (error != ESP_OK) { pxa_raster_asset_release(*output); *output=nullptr; return Error("preview cache", error); }
        }
        espp4_a::CameraResizeOriented(source + camera.crop_x, camera.crop_width, kSensorHeight, kSensorWidth,
            pixels, width, height, !camera.portrait && width == camera.width ? camera.columns : nullptr, camera.portrait);
    }
    UpdateControls(pixels, width, height);
    pxa_raster_asset_finish_loading(*output);
    ++camera.frames; ++camera.previews;
    camera.sample_us += esp_timer_get_time() - begin;
    if (camera.previews % 120 == 0)
        ESP_LOGI(kTag, "pipeline: preview=%u capture=%u wait_avg_us=%llu sample_avg_us=%llu encode_avg_us=%llu extent=%ux%u preview_ppa=%u preview_intermediate_copies=%u jpeg_copies=0 stale_drops=%u csi_done=%lu errors=0x%lx",
            camera.previews, camera.captures, camera.wait_us / camera.previews,
            camera.sample_us / camera.previews, camera.captures ? camera.encode_us / camera.captures : 0, width, height,
            camera.ppa && !camera.ppa_failed, camera.ppa && !camera.ppa_failed ? 1u : 0u,
            camera.stale_drops, static_cast<unsigned long>(camera.completions.load()), static_cast<unsigned long>(camera.errors.load()));
    return PXA_STATUS_OK;
}
pxa_status_t Capture(void*, uint8_t* output, size_t capacity, size_t* size) {
    if (!output || !size || !camera.started) return PXA_STATUS_INVALID_ARGUMENT;
    *size = 0;
    pxa_raster_asset_release(camera.pending_preview); camera.pending_preview = nullptr;
    const uint16_t* source = nullptr;
    auto status = AcquireFrame(&source);
    if (status != PXA_STATUS_OK) return status;
    const auto preview_status = MakePreview(source, &camera.pending_preview);
    const auto* input = reinterpret_cast<const uint8_t*>(source);
    const bool native = camera.width == kSensorWidth && camera.height == kSensorHeight;
    const bool shared_preview = (camera.portrait ? camera.height : camera.width) == espp4_a::kCameraPreviewWidth;
    if (shared_preview) {
        if (preview_status != PXA_STATUS_OK) {
            camera.completed.store(nullptr, std::memory_order_release);
            return preview_status;
        }
        // One immutable Core image feeds both the display and JPEG input DMA.
        // Input DMA accepts the Core image's 64-byte alignment; only JPEG's
        // output bitstream requires the separate 128-byte-aligned buffer.
        pxa_asset_object_view_t view{};
        pxa_asset_object_view(camera.pending_preview, &view);
        input = view.data;
    }
    if (camera.scaled_stride) {
        if (!PpaImage(source, camera.scaled, camera.scaled_stride, camera.width, camera.height)) {
            if (camera.ppa) {
                const auto error = esp_cache_msync(const_cast<uint16_t*>(source), camera.stride, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                if (error != ESP_OK) { camera.completed.store(nullptr, std::memory_order_release); return Error("rotation cache", error); }
            }
            espp4_a::CameraResizeOriented(source + camera.crop_x, camera.crop_width, kSensorHeight, kSensorWidth,
                reinterpret_cast<uint16_t*>(camera.scaled), camera.width, camera.height, camera.columns, camera.portrait);
        }
        input = camera.scaled;
    }
    if (!native) camera.completed.store(nullptr, std::memory_order_release);
    jpeg_encode_cfg_t config{};
    config.width = camera.width; config.height = camera.height; config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    config.sub_sample = JPEG_DOWN_SAMPLING_YUV422; config.image_quality = camera.quality;
    uint32_t encoded = 0;
    const int64_t begin = esp_timer_get_time();
    const auto error = jpeg_encoder_process(camera.encoder, &config, input, camera.width * camera.height * 2,
        output, std::min(capacity, camera.jpeg_capacity), &encoded);
    camera.encode_us += esp_timer_get_time() - begin; ++camera.captures;
    if (camera.captures % 30 == 1)
        ESP_LOGI(kTag, "capture: %ux%u jpeg=%lu input=%s encode_us=%lld",
            camera.width, camera.height, static_cast<unsigned long>(encoded),
            native ? "CSI" : shared_preview ? "Core-preview" : "scaled", esp_timer_get_time() - begin);
    if (error != ESP_OK) status = Error("JPEG encode", error);
    else if (!encoded || encoded > capacity) status = PXA_STATUS_LIMIT_EXCEEDED;
    else {
        *size = encoded;
    }
    if (native) camera.completed.store(nullptr, std::memory_order_release);
    return status;
}
pxa_status_t Preview(void*, pxa_raster_asset_t** output) {
    if (!output) return PXA_STATUS_INVALID_ARGUMENT;
    *output = nullptr;
    if (camera.pending_preview) {
        *output = camera.pending_preview; camera.pending_preview = nullptr;
        return PXA_STATUS_OK;
    }
    const uint16_t* source = nullptr;
    auto status = AcquireFrame(&source);
    if (status != PXA_STATUS_OK) return status;
    status = MakePreview(source, output);
    camera.completed.store(nullptr, std::memory_order_release);
    return status;
}
}
bool Espp4AInitializeCamera() {
    const PxaBoardVisionDriver driver{nullptr, true, false, Prepare, Capture, Finish,
        kProfiles, sizeof(kProfiles) / sizeof(kProfiles[0]), PrepareProfile, Preview, PxaBoardVisionPreviewJpeg};
    return pxa_board_vision_port_initialize(&driver);
}
