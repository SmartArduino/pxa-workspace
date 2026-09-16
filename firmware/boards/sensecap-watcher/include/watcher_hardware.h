#pragma once

#include "watcher_audio.h"
#include "watcher_battery.h"
#include "watcher_io_expander.h"

#include <atomic>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_io_interface.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <iot_button.h>
#include <led_strip.h>
#include <lvgl.h>

struct pxsys_standard_system;
typedef struct pxsys_standard_system pxsys_standard_system_t;
struct pxsys_reference_lvgl;
typedef struct pxsys_reference_lvgl pxsys_reference_lvgl_t;

// Board hardware for the SenseCAP Watcher: SPD2010 QSPI display and touch,
// PCA9555 power rails, wheel (rotary volume + power/select key), battery gauge
// and the ES8311 speaker path.
class SensecapWatcherHardware {
public:
    bool Initialize();
    void AttachSystem(pxsys_standard_system_t* system,
                      pxsys_reference_lvgl_t* reference_ui);
    void SetWifiEnabled(bool enabled);
    void SetBrightness(uint8_t percent);
    void SetVolume(uint8_t percent);
    void PublishStatus();
    void ShowInitialFrame();

    lv_display_t* display() const { return display_; }
    uint8_t brightness() const { return brightness_.load(); }
    uint8_t volume() const { return audio_.volume(); }

    // Used by the wheel-button driver which runs below this class.
    bool PollPowerKey(bool* pressed);

private:
    // SPD2010 touch protocol shim over the i2c_master driver. The stock
    // IDF 5.3+ i2c panel IO cannot express the register-less read the touch
    // driver issues, so the board provides the two transactions it needs.
    struct TouchPanelIo {
        esp_lcd_panel_io_t base;
        i2c_master_dev_handle_t device;
    };
    static esp_err_t TouchPanelTxParam(esp_lcd_panel_io_t* io, int lcd_cmd,
                                       const void* param, size_t param_size);
    static esp_err_t TouchPanelRxParam(esp_lcd_panel_io_t* io, int lcd_cmd,
                                       void* param, size_t param_size);
    static esp_err_t TouchPanelTxColor(esp_lcd_panel_io_t* io, int lcd_cmd,
                                       const void* color, size_t color_size);
    static esp_err_t TouchPanelDel(esp_lcd_panel_io_t* io);
    static esp_err_t TouchPanelRegisterCallbacks(
        esp_lcd_panel_io_t* io, const esp_lcd_panel_io_callbacks_t* callbacks,
        void* context);

    bool InitializeI2c();
    bool InitializePower();
    bool InitializeRgbLed();
    bool InitializeDisplay();
    bool InitializeTouch();
    bool InitializeBattery();
    bool InitializeInputs();
    bool InitializeWifi();
    void EnterWifiProvisioning();
    void ScheduleStatusUpdate();
    void ToggleScreen();
    void WakeScreen();
    void AdjustVolume(int delta);
    void PowerOff();
    static void StatusTimer(void* context);
    static void StatusOnLvgl(void* context);
    static void OnKnobRotateUp(void* knob, void* context);
    static void OnKnobRotateDown(void* knob, void* context);
    static void OnPowerKeyPressDown(void* handle, void* context);
    static void OnPowerKeyClick(void* handle, void* context);
    static void OnPowerKeyDoubleClick(void* handle, void* context);
    static void OnPowerKeyLongPress(void* handle, void* context);

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_bus_handle_t touch_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    TouchPanelIo touch_io_ = {};
    lv_display_t* display_ = nullptr;
    lv_indev_t* touch_indev_ = nullptr;
    led_strip_handle_t rgb_led_ = nullptr;
    adc_oneshot_unit_handle_t adc_ = nullptr;
    void* knob_ = nullptr;
    button_handle_t power_key_ = nullptr;
    WatcherIoExpander io_expander_;
    WatcherBattery battery_;
    WatcherAudio audio_;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* reference_ui_ = nullptr;
    esp_timer_handle_t status_timer_ = nullptr;
    int64_t boot_time_us_ = 0;
    std::atomic<uint8_t> brightness_{75};
    std::atomic<bool> screen_enabled_{true};
    std::atomic<bool> power_key_woke_screen_{false};
    std::atomic<bool> status_update_pending_{false};
    std::atomic<bool> wifi_initialized_{false};
    std::atomic<bool> wifi_enabled_{true};
};
