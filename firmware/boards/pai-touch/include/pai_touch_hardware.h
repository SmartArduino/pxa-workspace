#pragma once

#include "rpc701_audio.h"

#include <atomic>
#include <driver/adc_types_legacy.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <pxadb/pxadb_service.h>

class AdcBatteryMonitor;
class AdcButton;
struct pxsys_standard_system;
typedef struct pxsys_standard_system pxsys_standard_system_t;
struct pxsys_reference_lvgl;
typedef struct pxsys_reference_lvgl pxsys_reference_lvgl_t;

class PaiTouchHardware {
public:
    bool Initialize();
    void AttachSystem(pxsys_standard_system_t* system,
                      pxsys_reference_lvgl_t* reference_ui);
    void SetWifiEnabled(bool enabled);
    void SetBrightness(uint8_t percent);
    void SetVolume(uint8_t percent);
    void PublishStatus();
    void ShowInitialFrame();
    bool ConfigurePxadbControls();
    bool RequestPowerOff();

    lv_display_t* display() const { return display_; }
    uint8_t brightness() const { return brightness_.load(); }
    uint8_t volume() const { return audio_.volume(); }

private:
    bool InitializeDisplay();
    bool InitializeTouch();
    bool InitializeAdc();
    bool InitializeWifi();
    void InitializeButtons();
    void ScheduleStatusUpdate();
    void SetScreenEnabled(bool enabled);
    void ToggleScreen();
    void HandlePowerButtonPressDown();
    void HandlePowerButtonPressUp();
    void HandlePowerButtonLongPress();
    bool IsTouchInteractionRecent() const;
    void OnLockChanged(bool locked);
    void NavigateBack();
    void EnterWifiProvisioning();
    void PowerOff();
    bool RouteInjectedPointerDown();
    bool InjectPointer(uint16_t x, uint16_t y, bool pressed);
    bool CancelInjectedPointer();
    bool RouteInjectedKey(pxadb::TestControlKey key);
    bool CaptureRgb565(uint16_t* pixels, size_t pixel_count,
                       bool after_present,
                       pxadb::TestControlCaptureInfo* info);
    static void ReadInjectedPointer(lv_indev_t* indev, lv_indev_data_t* data);
    static void ReadPhysicalPointer(lv_indev_t* indev, lv_indev_data_t* data);
    static void StatusTimer(void* context);
    static void StatusOnLvgl(void* context);

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_display_t* display_ = nullptr;
    lv_indev_t* physical_pointer_ = nullptr;
    lv_indev_t* injected_pointer_ = nullptr;
    lv_indev_read_cb_t physical_pointer_read_cb_ = nullptr;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    AdcBatteryMonitor* battery_ = nullptr;
    AdcButton* power_button_ = nullptr;
    AdcButton* home_button_ = nullptr;
    AdcButton* volume_up_button_ = nullptr;
    AdcButton* volume_down_button_ = nullptr;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* reference_ui_ = nullptr;
    esp_timer_handle_t status_timer_ = nullptr;
    Rpc701Audio audio_;
    std::atomic<uint8_t> brightness_{75};
    std::atomic<bool> screen_enabled_{true};
    std::atomic<bool> wifi_initialized_{false};
    std::atomic<bool> wifi_enabled_{true};
    std::atomic<bool> status_update_pending_{false};
    std::atomic<uint16_t> injected_pointer_x_{0};
    std::atomic<uint16_t> injected_pointer_y_{0};
    std::atomic<bool> injected_pointer_pressed_{false};
    std::atomic<int64_t> last_touch_activity_us_{0};
    int64_t power_button_pressed_at_us_ = 0;
    bool power_button_long_press_ = false;
    bool power_button_woke_screen_ = false;
    bool power_button_ignored_ = false;
};
