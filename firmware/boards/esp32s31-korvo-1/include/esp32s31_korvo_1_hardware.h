#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <driver/ppa.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch.h>
#include <esp_lv_decoder.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <iot_button.h>

#include "esp32s31_korvo_1_audio.h"

struct pxsys_standard_system;
typedef struct pxsys_standard_system pxsys_standard_system_t;
struct pxsys_reference_lvgl;
typedef struct pxsys_reference_lvgl pxsys_reference_lvgl_t;

class Esp32S31Korvo1Hardware {
public:
    bool Initialize();
    void AttachSystem(pxsys_standard_system_t* system,
                      pxsys_reference_lvgl_t* reference_ui);
    void SetWifiEnabled(bool enabled);
    void SetVolume(uint8_t percent);
    uint8_t volume() const { return audio_.volume(); }
    lv_display_t* display() const { return display_; }

private:
    static constexpr int kTouchMaxPointers = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
    static constexpr size_t kDeferredSyncAreaCapacity = 32;

    bool InitializeI2c();
    bool InitializeDisplay();
    void SyncFrameBuffers(lv_display_t* display, const lv_area_t* area);
    void RecordDeferredSyncArea(const lv_area_t* area);
    void SyncReleasedFrameBuffer(void* next_frame_buffer);
    void CopyFrameBufferArea(void* source, void* destination,
                             const lv_area_t* area);
    bool InitializeTouch();
    void PollTouchController();
    static void TouchReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
    bool InitializeButtons();
    bool InitializeWifi();
    void AdjustVolume(int delta);
    void NavigateHome();
    void StartWifiProvisioning();
    void ScheduleStatusUpdate();
    void PublishStatus();
    void RecordRenderTime(uint32_t duration_us);
    void RecordDrawTime(uint32_t duration_us);
    void RecordPresentTime(uint32_t flush_us, uint32_t vsync_us,
                           bool vsync_ready);
    void RecordFrame(void);
    bool WaitForPendingFrame(void);
    void AlignAfterDirectScanout(void* next_buffer, void* displayed_buffer);
    static void StatusTimer(void* context);
    static void StatusOnLvgl(void* context);
    static void OnDisplayEvent(lv_event_t* event);
    static void FlushDisplay(lv_display_t* display, const lv_area_t* area,
                             uint8_t* pixels);
    static void SyncDisplay(lv_display_t* display, const lv_area_t* area);
    static bool OnFrameBufferComplete(
        esp_lcd_panel_handle_t panel,
        const esp_lcd_rgb_panel_event_data_t* event_data, void* context);
    static bool OnDirectScanoutTransition(bool entering, void* next_buffer,
                                          void* displayed_buffer,
                                          void* context);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    lv_indev_t* touch_indevs_[kTouchMaxPointers] = {};
    portMUX_TYPE touch_lock_ = portMUX_INITIALIZER_UNLOCKED;
    bool touch_slot_pressed_[kTouchMaxPointers] = {};
    uint8_t touch_slot_id_[kTouchMaxPointers] = {};
    lv_point_t touch_slot_point_[kTouchMaxPointers] = {};
    int64_t touch_last_report_us_ = 0;
    int64_t touch_last_poll_us_ = 0;
    esp_lv_decoder_handle_t image_decoder_ = nullptr;
    lv_display_t* display_ = nullptr;
    void* frame_buffers_[3] = {};
    lv_draw_buf_t draw_buffers_[3] = {};
    void* latest_frame_buffer_ = nullptr;
    lv_area_t deferred_sync_areas_[kDeferredSyncAreaCapacity] = {};
    size_t deferred_sync_area_count_ = 0;
    ppa_client_handle_t sync_ppa_client_ = nullptr;
    SemaphoreHandle_t frame_done_sem_ = nullptr;
    bool frame_switch_pending_ = false;
    int64_t render_started_us_ = 0;
    int64_t perf_report_started_us_ = 0;
    uint32_t perf_frames_ = 0;
    uint32_t perf_render_count_ = 0;
    uint32_t perf_draw_count_ = 0;
    uint32_t perf_sync_ppa_count_ = 0;
    uint32_t perf_sync_cpu_count_ = 0;
    uint32_t perf_vsync_timeouts_ = 0;
    uint64_t perf_render_total_us_ = 0;
    uint64_t perf_draw_total_us_ = 0;
    uint64_t perf_sync_total_us_ = 0;
    uint64_t perf_flush_total_us_ = 0;
    uint64_t perf_vsync_total_us_ = 0;
    uint32_t perf_render_max_us_ = 0;
    uint32_t perf_draw_max_us_ = 0;
    uint32_t perf_sync_max_us_ = 0;
    uint32_t perf_flush_max_us_ = 0;
    uint32_t perf_vsync_max_us_ = 0;
    uint64_t perf_flushes_ = 0;
    bool draw_measured_ = false;
    adc_oneshot_unit_handle_t button_adc_ = nullptr;
    button_handle_t buttons_[4] = {};
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* reference_ui_ = nullptr;
    esp_timer_handle_t status_timer_ = nullptr;
    std::atomic<bool> audio_initialized_{false};
    std::atomic<bool> wifi_initialized_{false};
    std::atomic<bool> wifi_enabled_{true};
    std::atomic<bool> status_update_pending_{false};
    Esp32S31Korvo1Audio audio_;
};
