#include "pai_touch_hardware.h"

#include "pai_touch_config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <adc_battery_monitor.h>
#include <button.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_common.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst826.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>
#include <wifi_manager.h>

#include "parallel_sw_rotation_flush.h"
#include "te_sync_panel.h"
#include "display/logical_pointer_coordinates.h"

namespace {
constexpr char kTag[] = "PaiTouchHw";
constexpr uint32_t kLcdClockHz = 80 * 1000 * 1000;
constexpr int kLcdTransferLines = 4;
constexpr int kLcdQueueDepth = 2;
constexpr uint32_t kTouchPollMs = 5;
// Release every contact when the controller stops answering, otherwise a
// failed I2C read would leave a pointer latched down.
constexpr int64_t kTouchReleaseTimeoutUs = 150 * 1000;
constexpr int64_t kPowerButtonMinPressUs = 50 * 1000;
constexpr int64_t kPowerButtonTouchGuardUs = 350 * 1000;
constexpr char kPerformanceNamespace[] = "pxa_perf";
constexpr char kPerformanceOverlayKey[] = "overlay";
constexpr char kPerformanceLogKey[] = "log";

struct LcdCommand {
    uint8_t command;
    uint8_t data[32];
    uint8_t length;
    uint16_t delay_ms;
};

// BOE WV020JAU-N80 / JD9853 supplier sequence dated 2024-05-17.
constexpr LcdCommand kLcdCommands[] = {
    {0x01, {}, 0, 10},
    {0xDF, {0x98, 0x53}, 2, 0},
    {0xDE, {0x00}, 1, 0},
    {0xB2, {0x25}, 1, 0},
    {0xB7, {0x00, 0x29, 0x00, 0x51}, 4, 0},
    {0xBB, {0x47, 0x1F, 0x46, 0x71, 0x73, 0xF0}, 6, 0},
    {0xBC, {0x77}, 1, 0},
    {0xC0, {0x44, 0xA4}, 2, 0},
    {0xC1, {0x12}, 1, 0},
    {0xC3, {0x7D, 0x07, 0x14, 0x06, 0xC8, 0x71, 0x6C, 0x77}, 8, 0},
    {0xC4, {0x08, 0x00, 0x94, 0xA9, 0x25, 0x0A, 0x16, 0x79,
            0x25, 0x0A, 0x16, 0x82}, 12, 0},
    {0xC8, {0x3F, 0x33, 0x2B, 0x25, 0x29, 0x28, 0x22, 0x21,
            0x20, 0x1F, 0x1E, 0x14, 0x0F, 0x0A, 0x06, 0x00,
            0x3F, 0x33, 0x2B, 0x25, 0x29, 0x28, 0x22, 0x21,
            0x20, 0x1F, 0x1E, 0x14, 0x0F, 0x0A, 0x06, 0x00}, 32, 0},
    {0xD0, {0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, {0x00, 0x30}, 2, 0},
    {0xE6, {0x10}, 1, 0},
    {0xDE, {0x01}, 1, 0},
    {0xB7, {0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, {0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, {0x06, 0x3A}, 2, 0},
    {0xC4, {0x72, 0x12}, 2, 0},
    {0xC5, {0x03}, 1, 0},
    {0xBB, {0x04}, 1, 0},
    {0xD7, {0x12}, 1, 0},
    {0xBE, {0x00}, 1, 0},
    {0xDE, {0x02}, 1, 0},
    {0xBD, {0x03, 0x53}, 2, 0},
    {0xDE, {0x00}, 1, 0},
    {0x35, {0x00}, 1, 0},
    {0x36, {0x00}, 1, 0},
    {0x3A, {0x05}, 1, 0},
    {0x2A, {0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0x27}, 4, 0},
    {0x11, {}, 0, 120},
    {0xDE, {0x03}, 1, 0},
    {0xB5, {0x23}, 1, 0},
    {0xDE, {0x00}, 1, 0},
    {0x29, {}, 0, 1},
};

uint8_t SignalLevel(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}
}  // namespace

bool PaiTouchHardware::PerformanceGet(
    pxsys_reference_performance_option_t option) const {
    switch (option) {
        case PXSYS_REFERENCE_PERFORMANCE_OVERLAY:
            return zuowei_pai_touch::ParallelSoftwareRotationFlush::
                PerformanceOverlayEnabled();
        case PXSYS_REFERENCE_PERFORMANCE_LOG:
            return zuowei_pai_touch::ParallelSoftwareRotationFlush::
                PerformanceLogEnabled();
        default:
            return false;
    }
}

bool PaiTouchHardware::PerformanceSet(
    pxsys_reference_performance_option_t option, bool enabled) {
    const char* key = nullptr;
    switch (option) {
        case PXSYS_REFERENCE_PERFORMANCE_OVERLAY:
            key = kPerformanceOverlayKey;
            break;
        case PXSYS_REFERENCE_PERFORMANCE_LOG:
            key = kPerformanceLogKey;
            break;
        default:
            return false;
    }
    nvs_handle_t handle;
    if (nvs_open(kPerformanceNamespace, NVS_READWRITE, &handle) != ESP_OK)
        return false;
    const esp_err_t result = nvs_set_u8(handle, key, enabled ? 1 : 0);
    const esp_err_t committed = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    if (committed != ESP_OK) return false;
    if (option == PXSYS_REFERENCE_PERFORMANCE_OVERLAY)
        zuowei_pai_touch::ParallelSoftwareRotationFlush::
            SetPerformanceOverlayEnabled(enabled);
    else
        zuowei_pai_touch::ParallelSoftwareRotationFlush::
            SetPerformanceLogEnabled(enabled);
    return true;
}

bool PaiTouchHardware::Initialize() {
    // Wake the JL701 before display and network startup can leave it idle.
    if (!audio_.Initialize()) ESP_LOGW(kTag, "Continuing without RPC701 audio");
    if (!InitializeDisplay()) return false;
    nvs_handle_t performance_handle;
    if (nvs_open(kPerformanceNamespace, NVS_READONLY, &performance_handle) ==
        ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(performance_handle, kPerformanceOverlayKey, &enabled) ==
            ESP_OK) {
            zuowei_pai_touch::ParallelSoftwareRotationFlush::
                SetPerformanceOverlayEnabled(enabled != 0);
        }
        if (nvs_get_u8(performance_handle, kPerformanceLogKey, &enabled) ==
            ESP_OK) {
            zuowei_pai_touch::ParallelSoftwareRotationFlush::
                SetPerformanceLogEnabled(enabled != 0);
        }
        nvs_close(performance_handle);
    }
    if (!InitializeTouch()) ESP_LOGW(kTag, "Continuing without touch input");
    if (!InitializeAdc()) return false;
    InitializeButtons();
    if (!InitializeWifi()) ESP_LOGW(kTag, "Continuing without Wi-Fi");

    const esp_timer_create_args_t timer_config = {
        .callback = StatusTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pxsys_status",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_config, &status_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer_, 1000 * 1000));
    return true;
}

bool PaiTouchHardware::InitializeDisplay() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 25000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = PAI_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = false},
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));

    spi_bus_config_t bus = {};
    bus.mosi_io_num = PAI_LCD_MOSI;
    bus.miso_io_num = GPIO_NUM_NC;
    bus.sclk_io_num = PAI_LCD_CLOCK;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = PAI_DISPLAY_HEIGHT * kLcdTransferLines * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = PAI_LCD_CS;
    io.dc_gpio_num = PAI_LCD_DC;
    io.spi_mode = 3;
    io.pclk_hz = kLcdClockHz;
    io.trans_queue_depth = kLcdQueueDepth;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    /* Rotation buffers live in aligned PSRAM. ESP32-S3 DMA can consume them
     * directly, avoiding two internal 1920-byte bounce buffers per frame. */
    io.flags.psram_dma_direct = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &panel_io_));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PAI_LCD_RESET;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
    vTaskDelay(pdMS_TO_TICKS(120));
    for (const auto& command : kLcdCommands) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(
            panel_io_, command.command,
            command.length == 0 ? nullptr : command.data, command.length));
        if (command.delay_ms != 0) vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    esp_lcd_panel_handle_t synchronized_panel = nullptr;
    ESP_ERROR_CHECK(CreateTeSynchronizedPanel(
        panel_io_, panel_, PAI_LCD_TE, PAI_DISPLAY_HEIGHT, PAI_DISPLAY_WIDTH,
        1, &synchronized_panel));
    panel_ = synchronized_panel;

    lv_init();
    const lvgl_port_cfg_t port = {
        .task_priority = 4,
        .task_stack = 10 * 1024,
        .task_affinity = 1,
        .task_max_sleep_ms = 100,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 2,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port));

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = PAI_DISPLAY_WIDTH * PAI_DISPLAY_HEIGHT,
        .double_buffer = false,
        .trans_size = 0,
        .hres = PAI_DISPLAY_HEIGHT,
        .vres = PAI_DISPLAY_WIDTH,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = true,
        },
    };
    if (!lvgl_port_lock(1000)) return false;
    lv_lock();
    display_ = lvgl_port_add_disp(&display_config);
    if (display_ != nullptr) {
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
        if (!zuowei_pai_touch::ParallelSoftwareRotationFlush::Install(
                display_, panel_io_, panel_)) {
            ESP_LOGW(kTag, "Parallel display rotation unavailable");
            lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_FULL);
        }
    }
    lv_unlock();
    lvgl_port_unlock();
    ESP_LOGI(kTag, "JD9853 display initialized as 296x240");
    return display_ != nullptr;
}

bool PaiTouchHardware::InitializeTouch() {
    i2c_master_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t touch_io = nullptr;
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = PAI_TOUCH_I2C_PORT,
        .sda_io_num = PAI_TOUCH_SDA,
        .scl_io_num = PAI_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    esp_err_t result = i2c_new_master_bus(&bus_config, &bus);
    if (result != ESP_OK) return false;

    esp_lcd_panel_io_i2c_config_t io = ESP_LCD_TOUCH_IO_I2C_CST826_CONFIG();
    io.scl_speed_hz = 400 * 1000;
    result = esp_lcd_new_panel_io_i2c(bus, &io, &touch_io);
    if (result != ESP_OK) return false;
    const esp_lcd_touch_config_t touch_config = {
        .x_max = PAI_DISPLAY_HEIGHT,
        .y_max = PAI_DISPLAY_WIDTH,
        .rst_gpio_num = PAI_TOUCH_RESET,
        .int_gpio_num = PAI_TOUCH_INTERRUPT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
    };
    result = esp_lcd_touch_new_i2c_cst826(touch_io, &touch_config, &touch_);
    if (result != ESP_OK) return false;
    if (PAI_TOUCH_INTERRUPT != GPIO_NUM_NC &&
        esp_lcd_touch_register_interrupt_callback_with_data(
            touch_, TouchInterrupt, this) != ESP_OK) {
        ESP_LOGW(kTag, "Touch interrupt unavailable, polling only");
    }

    // esp_lvgl_port publishes only the first point from a multi-touch report.
    // Give every CST826 contact a dedicated pointer indev instead, so the PXA
    // bridge emits one stable pointer_id per finger.
    if (!lvgl_port_lock(100)) return false;
    lv_lock();
    for (int i = 0; i < kTouchMaxPointers; ++i) {
        lv_indev_t* indev = lv_indev_create();
        if (indev == nullptr) continue;
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, ReadPhysicalPointer);
        lv_indev_set_disp(indev, display_);
        lv_indev_set_driver_data(indev, this);
        lv_indev_set_user_data(
            indev, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_timer_set_period(lv_indev_get_read_timer(indev), kTouchPollMs);
        physical_pointers_[i] = indev;
    }
    lv_unlock();
    lvgl_port_unlock();
    if (physical_pointers_[0] == nullptr) return false;
    ESP_LOGI(kTag, "CST826 ready: %d independent touch pointers",
             kTouchMaxPointers);
    return true;
}

bool PaiTouchHardware::InitializeAdc() {
    const adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = PAI_BUTTON_ADC_UNIT,
    };
    if (adc_oneshot_new_unit(&adc_config, &adc_) != ESP_OK) return false;
    battery_ = new AdcBatteryMonitor(
        ADC_UNIT_1, PAI_BATTERY_ADC_CHANNEL, 5100.0f, 10000.0f,
        PAI_BATTERY_CHARGING, adc_);
    return battery_ != nullptr;
}

void PaiTouchHardware::InitializeButtons() {
    button_adc_config_t config = {};
    config.adc_channel = PAI_BUTTON_ADC_CHANNEL;
    config.adc_handle = &adc_;

    config.button_index = 0;
    config.min = PAI_BUTTON_POWER_MIN;
    config.max = PAI_BUTTON_POWER_MAX;
    power_button_ = new AdcButton(config, 1500);
    config.button_index = 1;
    config.min = PAI_BUTTON_HOME_MIN;
    config.max = PAI_BUTTON_HOME_MAX;
    home_button_ = new AdcButton(config, 1500);
    config.button_index = 2;
    config.min = PAI_BUTTON_UP_MIN;
    config.max = PAI_BUTTON_UP_MAX;
    volume_up_button_ = new AdcButton(config);
    config.button_index = 3;
    config.min = PAI_BUTTON_DOWN_MIN;
    config.max = PAI_BUTTON_DOWN_MAX;
    volume_down_button_ = new AdcButton(config);

    power_button_->OnPressDown([this]() { HandlePowerButtonPressDown(); });
    power_button_->OnPressUp([this]() { HandlePowerButtonPressUp(); });
    power_button_->OnLongPress([this]() { HandlePowerButtonLongPress(); });
    home_button_->OnClick([this]() { NavigateBack(); });
    home_button_->OnLongPress([this]() { EnterWifiProvisioning(); });
    volume_up_button_->OnPressDown([this]() {
        if (!pxa_host_captures_volume_keys() ||
            !pxa_host_post_key(PXA_HOST_KEY_VOLUME_UP)) {
            SetVolume(std::min<int>(100, volume() + 10));
        }
    });
    volume_up_button_->OnPressUp([]() {
        if (pxa_host_captures_volume_keys())
            (void)pxa_host_post_key(PXA_HOST_KEY_VOLUME_UP_RELEASED);
    });
    volume_down_button_->OnPressDown([this]() {
        if (!pxa_host_captures_volume_keys() ||
            !pxa_host_post_key(PXA_HOST_KEY_VOLUME_DOWN)) {
            SetVolume(volume() > 10 ? volume() - 10 : 0);
        }
    });
    volume_down_button_->OnPressUp([]() {
        if (pxa_host_captures_volume_keys())
            (void)pxa_host_post_key(PXA_HOST_KEY_VOLUME_DOWN_RELEASED);
    });
}

bool PaiTouchHardware::InitializeWifi() {
    WifiManagerConfig config;
    config.ssid_prefix = "PaiTouch";
    config.language = "zh-CN";
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected || event == WifiEvent::Disconnected ||
            event == WifiEvent::ConfigModeEnter ||
            event == WifiEvent::ConfigModeExit) {
            ScheduleStatusUpdate();
        }
    });
    if (!wifi.Initialize(config)) return false;
    wifi_initialized_.store(true);
    wifi.StartStation();
    return true;
}

void PaiTouchHardware::AttachSystem(pxsys_standard_system_t* system,
                                    pxsys_reference_lvgl_t* reference_ui) {
    system_ = system;
    reference_ui_ = reference_ui;
    if (reference_ui_ != nullptr) {
        (void)pxsys_reference_lvgl_set_lock_changed_callback(
            reference_ui_, this,
            [](void* context, bool locked) {
                static_cast<PaiTouchHardware*>(context)->OnLockChanged(locked);
            });
        (void)pxsys_reference_lvgl_set_power_action_callback(
            reference_ui_, this,
            [](void* context, pxsys_reference_power_action_t action) {
                auto* self = static_cast<PaiTouchHardware*>(context);
                if (action == PXSYS_REFERENCE_POWER_ACTION_SHUTDOWN) {
                    (void)self->RequestPowerOff();
                    return;
                }
                if (xTaskCreate(
                        [](void*) {
                            vTaskDelay(pdMS_TO_TICKS(120));
                            esp_restart();
                        },
                        "system_restart", 2048, nullptr, 4, nullptr) !=
                    pdPASS) {
                    ESP_LOGE(kTag, "Cannot create restart task");
                }
            });
        (void)pxsys_reference_lvgl_set_power_menu_changed_callback(
            reference_ui_, nullptr,
            [](void*, bool visible) {
                pxa_esp_surface_set_system_overlay_visible(visible);
            });
    }
    PublishStatus();
}

void PaiTouchHardware::SetWifiEnabled(bool enabled) {
    if (!wifi_initialized_.load()) return;
    if (wifi_enabled_.exchange(enabled) == enabled) return;
    auto& wifi = WifiManager::GetInstance();
    if (enabled) {
        wifi.StartStation();
    } else {
        wifi.StopConfigAp();
        wifi.StopStation();
    }
    ScheduleStatusUpdate();
}

void PaiTouchHardware::SetBrightness(uint8_t percent) {
    percent = std::min<uint8_t>(percent, 100);
    brightness_.store(percent);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                  screen_enabled_.load()
                      ? static_cast<uint32_t>(percent) * 1023 / 100
                      : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ScheduleStatusUpdate();
}

void PaiTouchHardware::SetVolume(uint8_t percent) {
    audio_.SetVolume(percent);
    ScheduleStatusUpdate();
}

void PaiTouchHardware::PublishStatus() {
    if (system_ == nullptr) return;
    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    const std::time_t now = std::time(nullptr);
    struct tm local = {};
    if (localtime_r(&now, &local) != nullptr && local.tm_year + 1900 >= 2020) {
        status.time_valid = 1;
        status.hour = local.tm_hour;
        status.minute = local.tm_min;
        status.date_valid = 1;
        status.year = local.tm_year + 1900;
        status.month = local.tm_mon + 1;
        status.day = local.tm_mday;
        status.weekday = local.tm_wday;
    }
    if (battery_ != nullptr) {
        uint8_t level = 0;
        status.battery_valid = battery_->GetBatteryLevel(level);
        status.battery_percent = level;
        status.charging = battery_->IsCharging();
    }
    auto& wifi = WifiManager::GetInstance();
    const bool connected = wifi.IsConnected();
    status.network_connected = connected;
    status.network_type = connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    status.network_signal_level = connected ? SignalLevel(wifi.GetRssi()) : 0;
    status.wifi_supported = wifi_initialized_.load();
    status.wifi_enabled = wifi_initialized_.load() && wifi_enabled_.load();
    status.wifi_connected = connected;
    status.wifi_signal_level = status.network_signal_level;
    status.volume_supported = 1;
    status.volume_percent = volume();
    status.brightness_supported = 1;
    status.brightness_percent = brightness();
    (void)pxsys_system_status_service_update(
        pxsys_standard_system_status(system_), &status);
}

void PaiTouchHardware::ShowInitialFrame() {
    if (!lvgl_port_lock(1000)) return;
    lv_lock();
    lv_refr_now(display_);
    lv_unlock();
    lvgl_port_unlock();
    (void)zuowei_pai_touch::ParallelSoftwareRotationFlush::WaitForPendingTransfers(
        pdMS_TO_TICKS(250));
    SetBrightness(75);
}

void PaiTouchHardware::ReadInjectedPointer(lv_indev_t* indev,
                                           lv_indev_data_t* data) {
    auto* self = static_cast<PaiTouchHardware*>(lv_indev_get_user_data(indev));
    if (self == nullptr || data == nullptr) return;
    data->timestamp = lv_tick_get();
    const int32_t logical_x =
        self->injected_pointer_x_.load(std::memory_order_relaxed);
    const int32_t logical_y =
        self->injected_pointer_y_.load(std::memory_order_relaxed);
    const int32_t original_width =
        lv_display_get_original_horizontal_resolution(self->display_);
    const int32_t original_height =
        lv_display_get_original_vertical_resolution(self->display_);

    display_input::Rotation rotation = display_input::Rotation::k0;
    switch (lv_display_get_rotation(self->display_)) {
        case LV_DISPLAY_ROTATION_90:
            rotation = display_input::Rotation::k90;
            break;
        case LV_DISPLAY_ROTATION_180:
            rotation = display_input::Rotation::k180;
            break;
        case LV_DISPLAY_ROTATION_270:
            rotation = display_input::Rotation::k270;
            break;
        default:
            break;
    }
    // LVGL rotates pointer samples after the read callback. PXADB coordinates
    // are already logical, so provide the inverse-rotated raw point here.
    const display_input::Point raw = display_input::LogicalToRaw(
        logical_x, logical_y, original_width, original_height, rotation);
    data->point.x = raw.x;
    data->point.y = raw.y;
    data->state = self->injected_pointer_pressed_.load(
                      std::memory_order_acquire)
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

void IRAM_ATTR PaiTouchHardware::TouchInterrupt(esp_lcd_touch_handle_t tp) {
    auto* self = static_cast<PaiTouchHardware*>(tp->config.user_data);
    if (self == nullptr) return;
    self->touch_irq_pending_.store(true, std::memory_order_release);
    // Wake the LVGL task so all pointer indevs sample the new report now
    // instead of waiting for their next polling period.
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, nullptr);
}

void PaiTouchHardware::ReadPhysicalPointer(lv_indev_t* indev,
                                           lv_indev_data_t* data) {
    auto* self = static_cast<PaiTouchHardware*>(lv_indev_get_driver_data(indev));
    if (self == nullptr || data == nullptr) return;
    const int slot = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_indev_get_user_data(indev)));
    if (slot < 0 || slot >= kTouchMaxPointers) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->timestamp = lv_tick_get();

    // All indevs share one controller report. The first callback in each
    // polling window refreshes that snapshot; the remaining callbacks merely
    // publish their own slot and never consume another report.
    const int64_t now_us = esp_timer_get_time();
    if (self->touch_irq_pending_.exchange(false, std::memory_order_acquire) ||
        now_us - self->touch_last_poll_us_ >=
            static_cast<int64_t>(kTouchPollMs) * 1000) {
        self->PollTouchController();
    }

    portENTER_CRITICAL(&self->touch_lock_);
    const bool pressed =
        self->touch_slot_pressed_[slot] && self->screen_enabled_.load();
    data->point = self->touch_slot_point_[slot];
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&self->touch_lock_);
    if (pressed != self->touch_logged_pressed_[slot]) {
        self->touch_logged_pressed_[slot] = pressed;
        ESP_LOGI(kTag, "lvgl pointer %d %s x=%d y=%d", slot,
                 pressed ? "down" : "up", (int)data->point.x,
                 (int)data->point.y);
    }
    if (pressed) self->last_touch_activity_us_.store(now_us);
}

void PaiTouchHardware::PollTouchController() {
    touch_last_poll_us_ = esp_timer_get_time();
    if (touch_ == nullptr) return;
    if (esp_lcd_touch_read_data(touch_) != ESP_OK) {
        if (touch_last_poll_us_ - touch_last_report_us_ >=
            kTouchReleaseTimeoutUs) {
            portENTER_CRITICAL(&touch_lock_);
            for (int slot = 0; slot < kTouchMaxPointers; ++slot) {
                touch_slot_pressed_[slot] = false;
            }
            portEXIT_CRITICAL(&touch_lock_);
        }
        return;
    }
    touch_last_report_us_ = touch_last_poll_us_;

    esp_lcd_touch_point_data_t points[kTouchMaxPointers] = {};
    uint8_t point_count = 0;
    if (esp_lcd_touch_get_data(touch_, points, &point_count,
                               kTouchMaxPointers) != ESP_OK) {
        return;
    }
    if (point_count > kTouchMaxPointers) point_count = kTouchMaxPointers;

    // Log contact-count transitions only; the poll runs at 200 Hz.
    if (point_count != touch_reported_points_) {
        touch_reported_points_ = point_count;
        ESP_LOGI(kTag,
                 "touch contacts=%u p0=(x=%u y=%u id=%u) p1=(x=%u y=%u id=%u)",
                 point_count, points[0].x, points[0].y, points[0].track_id,
                 points[1].x, points[1].y, points[1].track_id);
    }

    bool slot_seen[kTouchMaxPointers] = {};
    portENTER_CRITICAL(&touch_lock_);
    for (uint8_t i = 0; i < point_count; ++i) {
        const uint8_t track_id = points[i].track_id;
        int slot = -1;
        for (int candidate = 0; candidate < kTouchMaxPointers; ++candidate) {
            if (touch_slot_pressed_[candidate] && !slot_seen[candidate] &&
                touch_slot_id_[candidate] == track_id) {
                slot = candidate;
                break;
            }
        }
        for (int candidate = 0; slot < 0 && candidate < kTouchMaxPointers;
             ++candidate) {
            if (!touch_slot_pressed_[candidate] && !slot_seen[candidate]) {
                slot = candidate;
                break;
            }
        }
        if (slot < 0) continue;
        slot_seen[slot] = true;
        touch_slot_pressed_[slot] = true;
        touch_slot_id_[slot] = track_id;
        touch_slot_point_[slot].x = points[i].x;
        touch_slot_point_[slot].y = points[i].y;
    }
    for (int slot = 0; slot < kTouchMaxPointers; ++slot) {
        if (!slot_seen[slot]) touch_slot_pressed_[slot] = false;
    }
    portEXIT_CRITICAL(&touch_lock_);
}

bool PaiTouchHardware::RouteInjectedPointerDown() {
    if (screen_enabled_.load()) return true;
    SetScreenEnabled(true);
    return false;
}

bool PaiTouchHardware::InjectPointer(uint16_t x, uint16_t y, bool pressed) {
    if (display_ == nullptr || x >= PAI_DISPLAY_WIDTH ||
        y >= PAI_DISPLAY_HEIGHT) {
        return false;
    }
    injected_pointer_x_.store(x, std::memory_order_relaxed);
    injected_pointer_y_.store(y, std::memory_order_relaxed);
    injected_pointer_pressed_.store(pressed, std::memory_order_release);
    /* The injected indev's read timer runs lv_indev_read() inside the LVGL
     * task, which owns the display pipeline. Reading it here would run UI
     * event callbacks on the PXADB input task while holding the LVGL lock;
     * opening a heavy screen then stalls the whole UI for seconds. */
    if (injected_pointer_ != nullptr) return true;
    if (!lvgl_port_lock(1000)) return false;
    lv_lock();
    if (injected_pointer_ == nullptr) {
        injected_pointer_ = lv_indev_create();
        if (injected_pointer_ != nullptr) {
            lv_indev_set_type(injected_pointer_, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(injected_pointer_, display_);
            lv_indev_set_user_data(injected_pointer_, this);
            lv_indev_set_read_cb(injected_pointer_, ReadInjectedPointer);
            lv_timer_set_period(lv_indev_get_read_timer(injected_pointer_),
                                kTouchPollMs);
        }
    }
    const bool ready = injected_pointer_ != nullptr;
    lv_unlock();
    lvgl_port_unlock();
    return ready;
}

bool PaiTouchHardware::CancelInjectedPointer() {
    injected_pointer_pressed_.store(false, std::memory_order_release);
    return injected_pointer_ != nullptr;
}

bool PaiTouchHardware::RouteInjectedKey(pxadb::TestControlKey key) {
    switch (key) {
        case pxadb::TestControlKey::kBack:
        case pxadb::TestControlKey::kHome:
            NavigateBack();
            return true;
        case pxadb::TestControlKey::kVolumeUp:
            SetVolume(std::min<int>(100, volume() + 10));
            return true;
        case pxadb::TestControlKey::kVolumeDown:
            SetVolume(volume() > 10 ? volume() - 10 : 0);
            return true;
    }
    return false;
}

bool PaiTouchHardware::CaptureRgb565(
    uint16_t* pixels, size_t pixel_count, bool after_present,
    pxadb::TestControlCaptureInfo* info) {
    if (info == nullptr) return false;
    zuowei_pai_touch::ParallelSoftwareRotationFlush::CompletedFrameInfo frame;
    if (!zuowei_pai_touch::ParallelSoftwareRotationFlush::SnapshotCompletedFrame(
            pixels, pixel_count, after_present, pdMS_TO_TICKS(1000), &frame)) {
        return false;
    }
    info->width = frame.width;
    info->height = frame.height;
    info->stride_bytes = frame.stride_bytes;
    info->frame_id = frame.frame_id;
    info->completed_timestamp_us = frame.completed_timestamp_us;
    info->source = frame.source;
    return true;
}

bool PaiTouchHardware::ConfigurePxadbControls() {
    pxadb::PowerControlAdapter power_adapter;
    power_adapter.context = this;
    power_adapter.power_off = [](void* context) {
        return static_cast<PaiTouchHardware*>(context)->RequestPowerOff();
    };
    if (pxadb::ConfigurePowerControl(&power_adapter) != ESP_OK) return false;

    pxadb::TestControlAdapter adapter;
    adapter.context = this;
    adapter.width = PAI_DISPLAY_WIDTH;
    adapter.height = PAI_DISPLAY_HEIGHT;
    adapter.route_pointer_down = [](void* context) {
        return static_cast<PaiTouchHardware*>(context)
            ->RouteInjectedPointerDown();
    };
    adapter.inject_pointer = [](void* context, uint16_t x, uint16_t y,
                                bool pressed) {
        return static_cast<PaiTouchHardware*>(context)
            ->InjectPointer(x, y, pressed);
    };
    adapter.cancel_pointer = [](void* context) {
        return static_cast<PaiTouchHardware*>(context)
            ->CancelInjectedPointer();
    };
    adapter.route_key = [](void* context, pxadb::TestControlKey key) {
        return static_cast<PaiTouchHardware*>(context)->RouteInjectedKey(key);
    };
    adapter.capture_rgb565 = [](
        void* context, uint16_t* pixels, size_t pixel_count,
        bool after_present, pxadb::TestControlCaptureInfo* info) {
        return static_cast<PaiTouchHardware*>(context)->CaptureRgb565(
            pixels, pixel_count, after_present, info);
    };
    const esp_err_t result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}

bool PaiTouchHardware::RequestPowerOff() {
    if (xTaskCreate(
            [](void* context) {
                static_cast<PaiTouchHardware*>(context)->PowerOff();
            },
            "power_off", 3072, this, 4, nullptr) == pdPASS)
        return true;
    ESP_LOGE(kTag, "Cannot create power-off task");
    return false;
}

void PaiTouchHardware::ScheduleStatusUpdate() {
    if (system_ == nullptr || status_update_pending_.exchange(true)) return;
    lv_lock();
    lv_async_call(StatusOnLvgl, this);
    lv_unlock();
}

void PaiTouchHardware::StatusTimer(void* context) {
    static_cast<PaiTouchHardware*>(context)->ScheduleStatusUpdate();
}

void PaiTouchHardware::StatusOnLvgl(void* context) {
    auto* self = static_cast<PaiTouchHardware*>(context);
    self->status_update_pending_.store(false);
    self->PublishStatus();
}

void PaiTouchHardware::SetScreenEnabled(bool enabled) {
    const bool previous = screen_enabled_.exchange(enabled);
    if (previous == enabled) return;
    pxa_esp_surface_set_host_visible(false);
    if (!enabled) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    lv_lock();
    const lv_result_t result = lv_async_call(
        [](void* context) {
            auto* self = static_cast<PaiTouchHardware*>(context);
            if (self->reference_ui_ != nullptr)
                (void)pxsys_reference_lvgl_set_locked(self->reference_ui_, true);
            if (!self->screen_enabled_.load()) return;
            if (self->display_ != nullptr) lv_refr_now(self->display_);
            self->SetBrightness(self->brightness_.load() == 0
                                    ? 75
                                    : self->brightness_.load());
        },
        this);
    lv_unlock();
    if (result != LV_RESULT_OK)
        ESP_LOGE(kTag, "Cannot schedule screen %s",
                 enabled ? "wake" : "lock");
}

void PaiTouchHardware::ToggleScreen() {
    SetScreenEnabled(!screen_enabled_.load());
}

bool PaiTouchHardware::IsTouchInteractionRecent() const {
    const int64_t last_touch_us = last_touch_activity_us_.load();
    return last_touch_us != 0 &&
           esp_timer_get_time() - last_touch_us <= kPowerButtonTouchGuardUs;
}

void PaiTouchHardware::HandlePowerButtonPressDown() {
    power_button_woke_screen_ = !screen_enabled_.load();
    power_button_pressed_at_us_ = esp_timer_get_time();
    power_button_long_press_ = false;
    power_button_ignored_ =
        !power_button_woke_screen_ && IsTouchInteractionRecent();
    if (power_button_woke_screen_) SetScreenEnabled(true);
}

void PaiTouchHardware::HandlePowerButtonLongPress() {
    if (power_button_woke_screen_ || power_button_ignored_ ||
        IsTouchInteractionRecent()) {
        power_button_ignored_ = true;
        return;
    }
    power_button_long_press_ = true;
    lv_lock();
    const lv_result_t result = lv_async_call(
        [](void* context) {
            auto* self = static_cast<PaiTouchHardware*>(context);
            if (self->reference_ui_ != nullptr)
                (void)pxsys_reference_lvgl_show_power_menu(self->reference_ui_);
        },
        this);
    lv_unlock();
    if (result != LV_RESULT_OK)
        ESP_LOGE(kTag, "Cannot schedule power menu");
}

void PaiTouchHardware::HandlePowerButtonPressUp() {
    const int64_t pressed_at_us = power_button_pressed_at_us_;
    const int64_t duration_us = pressed_at_us == 0
                                    ? 0
                                    : esp_timer_get_time() - pressed_at_us;
    power_button_pressed_at_us_ = 0;
    if (power_button_woke_screen_) {
        power_button_woke_screen_ = false;
        power_button_ignored_ = false;
        return;
    }
    if (power_button_long_press_) {
        power_button_long_press_ = false;
        power_button_ignored_ = false;
        return;
    }
    if (power_button_ignored_ || IsTouchInteractionRecent() ||
        duration_us < kPowerButtonMinPressUs) {
        ESP_LOGW(kTag, "Ignored power-button ADC event: duration_us=%lld",
                 static_cast<long long>(duration_us));
        power_button_ignored_ = false;
        return;
    }
    ESP_LOGI(kTag, "Power-button short press: duration_ms=%lld",
             static_cast<long long>(duration_us / 1000));
    ToggleScreen();
}

void PaiTouchHardware::OnLockChanged(bool locked) {
    pxa_esp_surface_set_host_visible(!locked);
}

void PaiTouchHardware::NavigateBack() {
    if (!screen_enabled_.load()) {
        SetScreenEnabled(true);
        return;
    }
    if (reference_ui_ != nullptr &&
        pxsys_reference_lvgl_is_locked(reference_ui_))
        return;
    lv_lock();
    lv_async_call([](void* context) {
        auto* self = static_cast<PaiTouchHardware*>(context);
        if (self->reference_ui_ != nullptr)
            (void)pxsys_reference_lvgl_home(self->reference_ui_);
    }, this);
    lv_unlock();
}

void PaiTouchHardware::EnterWifiProvisioning() {
    if (!wifi_initialized_.load()) return;
    wifi_enabled_.store(true);
    WifiManager::GetInstance().StartConfigAp();
    ESP_LOGI(kTag, "Wi-Fi provisioning AP requested (PaiTouch-xxxx)");
}

void PaiTouchHardware::PowerOff() {
    ESP_LOGI(kTag, "Holding controller power-off signal on GPIO1");
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = PAI_POWER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 512,
        .hpoint = 0,
        .flags = {.output_invert = false},
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    vTaskDelay(pdMS_TO_TICKS(600));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    gpio_reset_pin(PAI_POWER_GPIO);
    gpio_set_direction(PAI_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PAI_POWER_GPIO, 0);
    for (;;) vTaskDelay(portMAX_DELAY);
}
