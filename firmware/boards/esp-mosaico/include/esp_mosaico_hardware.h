#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <iot_button.h>
#include <lvgl.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>
#include <pxadb/pxadb_service.h>

#include "esp_mosaico_audio.h"
#include "esp_mosaico_config.h"
#include "esp_mosaico_touch_events.h"
#include "esp_mosaico_touch_state.h"
#include "esp_mosaico_touch_slots.h"
#include "esp_mosaico_touch_queue.h"
#include "esp_mosaico_transfer.h"
#include "esp_mosaico_wake_touch.h"

class EspMosaicoHardware {
public:
    bool Initialize();
    void AttachSystem(pxsys_standard_system_t* system,
                      pxsys_reference_lvgl_t* reference_ui);
    void ShowInitialFrame();
    bool SetWifiEnabled(bool enabled);
    bool SetVolume(uint8_t percent);
    bool SetBrightness(uint8_t percent);
    void SetIdleDim(bool enabled, uint8_t percent);
    void IdleScreenOff();
    bool CaptureDisplayedRgb565(uint16_t* pixels, size_t pixel_count);
    bool ConfigurePxadbControls();
    lv_display_t* display() const { return display_; }

private:
    bool DetectHardware();
    bool InitializeUsb();
    bool InitializePower();
    bool InitializeI2c();
    bool InitializeDisplay();
    void ShowBootFrame();
    void WakePanel();
    bool InitializeTe();
    void WaitForTe(const lv_area_t& area);
    static void OnTe(void* context);
    bool InitializeTouch();
    bool InitializeButton();
    bool InitializeWifi();
    bool ApplyBrightness();
    bool WaitForTransfer(TickType_t timeout);
    void WaitForFlush();
    bool QueuePresent(const lv_area_t& area, uint64_t timestamp,
                      uint64_t frame_id = 0, uint32_t raster_ready_tick_us = 0);
    static void FlushTask(void* context);
    bool PresentDirectFrame();
    bool TransferArea(const lv_area_t& area, const uint8_t* pixels,
                       uint32_t stride_bytes, bool last, uint64_t input_timestamp,
                       const char* source, uint64_t frame_id);
    bool RouteInjectedPointerDown();
    bool InjectPointer(uint16_t x, uint16_t y, bool pressed);
    bool RouteInjectedKey(pxadb::TestControlKey key);
    bool CaptureRgb565(uint16_t* pixels, size_t pixel_count, bool after_present,
                        pxadb::TestControlCaptureInfo* info);
    static void ReadInjectedPointer(lv_indev_t* input, lv_indev_data_t* data);
    void PublishStatus();
    void PublishDiagnostics();
    static void PollTouch(void* context);
    static void ReadTouch(lv_indev_t* input, lv_indev_data_t* data);
    static void FlushDisplay(lv_display_t* display, const lv_area_t* area,
                             uint8_t* pixels);
    static void RoundDisplayArea(lv_area_t* area);
    static bool OnColorTransferDone(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t* event,
                                    void* context);

    std::optional<mosaico_board::HardwareConfig> hardware_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    lv_indev_t* touch_input_ = nullptr;
    struct TouchInputContext {
        EspMosaicoHardware* owner;
        size_t slot;
    };
    TouchInputContext touch_contexts_[2] = {{this, 0}, {this, 1}};
    lv_indev_t* secondary_touch_input_ = nullptr;
    lv_indev_t* injected_pointer_ = nullptr;
    std::atomic<uint16_t> injected_pointer_x_{0};
    std::atomic<uint16_t> injected_pointer_y_{0};
    std::atomic<bool> injected_pointer_pressed_{false};
    TaskHandle_t touch_task_ = nullptr;
    portMUX_TYPE touch_lock_ = portMUX_INITIALIZER_UNLOCKED;
    EspMosaicoTouchState touch_state_;
    EspMosaicoTouchEvents touch_events_;
    EspMosaicoTouchSlots touch_slots_;
    EspMosaicoTouchQueue touch_queue_[2];
    std::atomic<uint32_t> touch_reads_{0};
    std::atomic<uint32_t> touch_reports_{0};
    std::atomic<uint32_t> touch_errors_{0};
    std::atomic<int> touch_last_status_{ESP_OK};
    button_handle_t button_ = nullptr;
    lv_display_t* display_ = nullptr;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* reference_ui_ = nullptr;
    SemaphoreHandle_t transfer_done_ = nullptr;
    SemaphoreHandle_t te_ready_ = nullptr;
    std::atomic<uint32_t> te_edges_{0};
    std::atomic<uint32_t> te_timestamp_us_{0};
    std::atomic<uint32_t> te_rise_us_{0};
    std::atomic<uint32_t> te_blank_us_{0};
    std::atomic<uint32_t> te_period_us_{0};
    std::atomic<uint32_t> te_timeouts_{0};
    int64_t te_retry_us_ = 0;
    int64_t scan_deadline_us_ = 0; // Serialized by panel_mutex_.
    SemaphoreHandle_t present_available_ = nullptr;
    SemaphoreHandle_t panel_mutex_ = nullptr;
    struct FlushRequest {
        lv_area_t area;
        uint64_t timestamp;
        uint64_t frame_id;
        uint32_t raster_ready_tick_us;
    };
    bool PrepareDirectFrame(FlushRequest* request);
    bool flush_direct_checked_ = false;
    bool flush_direct_ready_ = false;
    FlushRequest flush_direct_request_ = {};
    QueueHandle_t flush_queue_ = nullptr;
    TaskHandle_t flush_task_ = nullptr;
    std::atomic<bool> redraw_pending_{false};
    uint8_t* transfer_pixels_[2] = {};
    bool transfer_pending_ = false;
    std::atomic<uint32_t> transfer_errors_{0};
    std::atomic<uint32_t> completed_frames_{0};
    std::atomic<uint32_t> direct_frames_{0};
    uint32_t diagnostics_logged_frames_ = 0;
    uint64_t displayed_frame_id_ = 0;
    uint64_t displayed_timestamp_us_ = 0;
    const char* displayed_source_ = "lvgl-composited";
    bool refresh_failed_ = false;
    bool displayed_frame_initialized_ = false;
    int64_t diagnostics_logged_us_ = 0;
    uint8_t* present_pixels_ = nullptr;
    uint8_t* scanout_pixels_ = nullptr; // Immutable while the worker owns it.
    bool present_pixels_current_ = true; // Restore once on direct -> LVGL.
    mosaico_board::RefreshRows pending_rows_; // Only with the display lock held.
    bool compose_failed_ = false;
    int64_t refresh_started_us_ = 0;
    uint32_t refresh_wait_us_ = 0; // Only with the display lock held.
    std::atomic<uint32_t> render_duration_us_{0};
    std::atomic<uint32_t> render_max_us_{0};
    std::atomic<uint32_t> present_wait_us_{0};
    std::atomic<uint32_t> direct_raster_us_{0};
    std::atomic<uint32_t> direct_compose_us_{0};
    std::atomic<uint32_t> direct_ppa_frames_{0};
    std::atomic<uint32_t> direct_cpu_frames_{0};
    std::atomic<uint32_t> direct_alpha_frames_{0};
    std::atomic<uint32_t> snapshot_us_{0};
    std::atomic<uint32_t> te_wait_us_{0};
    std::atomic<uint32_t> transfer_duration_us_{0};
    std::atomic<uint32_t> transfer_max_us_{0};
    std::atomic<uint32_t> scan_overruns_{0};
    bool displayed_frame_valid_ = false;
    std::atomic<bool> panel_on_{false};
    std::atomic<bool> wake_pending_{false};
    MosaicoWakeTouch wake_touch_;
    lv_obj_t* boot_screen_ = nullptr;
    bool audio_initialized_ = false;
    bool wifi_initialized_ = false;
    std::atomic<bool> time_sync_initialized_{false};
    std::atomic<bool> wifi_enabled_{true};
    std::atomic<uint8_t> brightness_{80};
    std::atomic<bool> idle_dim_enabled_{false};
    std::atomic<uint8_t> idle_dim_percent_{20};
    std::atomic<bool> home_pending_{false};
    std::atomic<bool> back_pending_{false};
    std::atomic<bool> provisioning_pending_{false};
    EspMosaicoAudio audio_;
};
