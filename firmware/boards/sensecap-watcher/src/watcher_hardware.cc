#include "watcher_hardware.h"

#include "sensecap_watcher_config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_spd2010.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_spd2010.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_knob.h>
#include <pxa/pxa_host.h>
#include <pxsys/standard_system.h>
#include <pxsys/system_status.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "WatcherHw";
// LVGL draw buffers live in PSRAM, so the SPI driver must bounce every color
// chunk through internal DMA memory. Keep each chunk small and serialize two
// at a time so the bounce buffers always fit; 20 lines is 16480 bytes.
constexpr int kLcdQueueDepth = 2;
constexpr int kLcdTransferLines = 20;
constexpr int kTouchPollMs = 8;
constexpr int kTouchI2cTimeoutMs = 100;
constexpr int kVolumeStep = 5;
constexpr uint8_t kDefaultBrightness = 75;
constexpr int64_t kPowerKeyBootGraceUs = 3 * 1000 * 1000;

uint8_t SignalLevel(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

// SPD2010 QSPI transfers want areas aligned to four pixels.
void RoundDisplayArea(lv_area_t* area) {
    if (area == nullptr) return;
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
}

// The wheel button lives on the PCA9555, so it is exposed to iot_button as a
// custom driver that samples the expander input register.
struct PowerKeyDriver {
    button_driver_t base;
    SensecapWatcherHardware* hardware;
};

uint8_t ReadPowerKeyLevel(button_driver_t* driver) {
    auto* self = reinterpret_cast<PowerKeyDriver*>(driver);
    bool pressed = false;
    if (self == nullptr || self->hardware == nullptr ||
        !self->hardware->PollPowerKey(&pressed)) {
        return 0;
    }
    return pressed ? 1 : 0;
}

esp_err_t DeletePowerKeyDriver(button_driver_t* driver) {
    free(reinterpret_cast<PowerKeyDriver*>(driver));
    return ESP_OK;
}
}  // namespace

bool SensecapWatcherHardware::Initialize() {
    boot_time_us_ = esp_timer_get_time();
    if (!InitializeI2c()) return false;
    if (!InitializePower()) return false;
    if (!InitializeRgbLed()) ESP_LOGW(kTag, "Continuing with the RGB LED on");
    if (!InitializeDisplay()) return false;
    if (!InitializeTouch()) ESP_LOGW(kTag, "Continuing without touch input");
    if (!InitializeBattery()) ESP_LOGW(kTag, "Continuing without battery data");
    if (!InitializeInputs()) ESP_LOGW(kTag, "Continuing without wheel input");
    if (!audio_.Initialize(i2c_bus_)) {
        ESP_LOGW(kTag, "Continuing without speaker output");
    }
    if (!InitializeWifi()) ESP_LOGW(kTag, "Continuing without Wi-Fi");

    const esp_timer_create_args_t timer_config = {
        .callback = StatusTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "watcher_status",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_config, &status_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer_,
                                             WATCHER_STATUS_TIMER_PERIOD_US));
    return true;
}

bool SensecapWatcherHardware::InitializeI2c() {
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = WATCHER_I2C_PORT,
        .sda_io_num = WATCHER_I2C_SDA,
        .scl_io_num = WATCHER_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&bus_config, &i2c_bus_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create I2C0 bus");
        return false;
    }

    // Hold the unpowered LCD and touch lines low while the rails settle, as
    // the factory firmware does before enabling the panel power.
    const gpio_config_t hold_low = {
        .pin_bit_mask = (1ULL << WATCHER_LCD_PCLK) |
                        (1ULL << WATCHER_LCD_DATA0) |
                        (1ULL << WATCHER_LCD_DATA1) |
                        (1ULL << WATCHER_LCD_DATA2) |
                        (1ULL << WATCHER_LCD_DATA3) |
                        (1ULL << WATCHER_LCD_CS) |
                        (1ULL << WATCHER_LCD_BACKLIGHT) |
                        (1ULL << WATCHER_TOUCH_SDA) |
                        (1ULL << WATCHER_TOUCH_SCL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&hold_low) != ESP_OK) return false;
    gpio_set_level(WATCHER_LCD_PCLK, 0);
    gpio_set_level(WATCHER_LCD_DATA0, 0);
    gpio_set_level(WATCHER_LCD_DATA1, 0);
    gpio_set_level(WATCHER_LCD_DATA2, 0);
    gpio_set_level(WATCHER_LCD_DATA3, 0);
    gpio_set_level(WATCHER_LCD_CS, 0);
    gpio_set_level(WATCHER_LCD_BACKLIGHT, 0);
    gpio_set_level(WATCHER_TOUCH_SDA, 0);
    gpio_set_level(WATCHER_TOUCH_SCL, 0);
    return true;
}

bool SensecapWatcherHardware::InitializePower() {
    if (!io_expander_.Initialize(i2c_bus_, WATCHER_IO_EXPANDER_ADDRESS,
                                 WATCHER_IO_INPUT_MASK,
                                 ~WATCHER_IO_INPUT_MASK)) {
        return false;
    }
    // Latch the system rail first; everything else follows.
    if (!io_expander_.SetOutputs(WATCHER_IO_PWR_SYSTEM, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!io_expander_.SetOutputs(
            WATCHER_IO_POWER_UP_MASK & ~WATCHER_IO_PWR_SYSTEM, true)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

bool SensecapWatcherHardware::InitializeRgbLed() {
    // Clear the WS2812 left latched by whichever firmware ran before us.
    const led_strip_config_t strip_config = {
        .strip_gpio_num = WATCHER_RGB_LED,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false},
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {.with_dma = false},
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &rgb_led_) !=
        ESP_OK) {
        rgb_led_ = nullptr;
        ESP_LOGE(kTag, "Cannot initialize the RGB LED");
        return false;
    }
    if (led_strip_clear(rgb_led_) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

bool SensecapWatcherHardware::InitializeDisplay() {
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = WATCHER_LCD_BACKLIGHT_DUTY_RES,
        .timer_num = WATCHER_LCD_BACKLIGHT_TIMER,
        .freq_hz = WATCHER_LCD_BACKLIGHT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = WATCHER_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = WATCHER_LCD_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = WATCHER_LCD_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = false},
    };
    if (ledc_timer_config(&backlight_timer) != ESP_OK ||
        ledc_channel_config(&backlight_channel) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot configure LCD backlight");
        return false;
    }

    const spi_bus_config_t bus_config = {
        .data0_io_num = WATCHER_LCD_DATA0,
        .data1_io_num = WATCHER_LCD_DATA1,
        .sclk_io_num = WATCHER_LCD_PCLK,
        .data2_io_num = WATCHER_LCD_DATA2,
        .data3_io_num = WATCHER_LCD_DATA3,
        .max_transfer_sz = WATCHER_DISPLAY_WIDTH * kLcdTransferLines * 2,
    };
    if (spi_bus_initialize(WATCHER_LCD_SPI_HOST, &bus_config,
                           SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot initialize LCD QSPI bus");
        return false;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = WATCHER_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 3,
        .pclk_hz = WATCHER_LCD_PIXEL_CLK_HZ,
        .trans_queue_depth = kLcdQueueDepth,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    if (esp_lcd_new_panel_io_spi(WATCHER_LCD_SPI_HOST, &io_config,
                                 &panel_io_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 panel IO");
        return false;
    }

    spd2010_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    if (esp_lcd_new_panel_spd2010(panel_io_, &panel_config, &panel_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 panel");
        return false;
    }
    if (esp_lcd_panel_reset(panel_) != ESP_OK ||
        esp_lcd_panel_init(panel_) != ESP_OK ||
        esp_lcd_panel_mirror(panel_, false, false) != ESP_OK ||
        esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot bring up SPD2010 panel");
        return false;
    }

    lv_init();
    const lvgl_port_cfg_t port_config = {
        .task_priority = 4,
        .task_stack = 10 * 1024,
        .task_affinity = 1,
        .task_max_sleep_ms = 100,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 2,
    };
    if (lvgl_port_init(&port_config) != ESP_OK) return false;

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        // The panel has no TE line, so vsync locking is impossible. Direct
        // mode keeps full-screen double buffers but renders and flushes only
        // the invalidated areas, which is far cheaper than full_refresh. The
        // native swapped format lets LVGL render byte-swapped directly: an
        // in-place swap is not allowed on a direct-mode framebuffer.
        .buffer_size = WATCHER_DISPLAY_WIDTH * WATCHER_DISPLAY_HEIGHT,
        .double_buffer = true,
        .trans_size = 0,
        .hres = WATCHER_DISPLAY_WIDTH,
        .vres = WATCHER_DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .rounder_cb = RoundDisplayArea,
        .color_format = LV_COLOR_FORMAT_RGB565_SWAPPED,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = true,
        },
    };
    display_ = lvgl_port_add_disp(&display_config);
    if (display_ == nullptr) {
        ESP_LOGE(kTag, "Cannot register SPD2010 display with LVGL");
        return false;
    }
    ESP_LOGI(kTag, "SPD2010 display initialized as %dx%d",
             WATCHER_DISPLAY_WIDTH, WATCHER_DISPLAY_HEIGHT);
    return true;
}

bool SensecapWatcherHardware::InitializeTouch() {
    // The SPD2010 touch driver issues register-less reads (a bare I2C receive
    // after a command write). The stock IDF 5.3+ i2c panel IO rejects the
    // resulting zero-length write phase, so the board exposes the two
    // transactions the driver needs through a small panel IO shim over the
    // i2c_master driver. Mixing in the legacy I2C driver is not an option:
    // IDF 5.5 aborts when both drivers are linked.
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = WATCHER_TOUCH_I2C_PORT,
        .sda_io_num = WATCHER_TOUCH_SDA,
        .scl_io_num = WATCHER_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true},
    };
    if (i2c_new_master_bus(&bus_config, &touch_bus_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create touch I2C bus");
        return false;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS,
        .scl_speed_hz = WATCHER_TOUCH_CLOCK_HZ,
    };
    if (i2c_master_bus_add_device(touch_bus_, &device_config,
                                  &touch_io_.device) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot attach the SPD2010 touch controller");
        return false;
    }
    touch_io_.base.tx_param = TouchPanelTxParam;
    touch_io_.base.rx_param = TouchPanelRxParam;
    touch_io_.base.tx_color = TouchPanelTxColor;
    touch_io_.base.del = TouchPanelDel;
    touch_io_.base.register_event_callbacks = TouchPanelRegisterCallbacks;
    const esp_lcd_panel_io_handle_t touch_io =
        reinterpret_cast<esp_lcd_panel_io_handle_t>(&touch_io_);

    const esp_lcd_touch_config_t touch_config = {
        .x_max = WATCHER_DISPLAY_WIDTH,
        .y_max = WATCHER_DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        .user_data = nullptr,
    };
    esp_lcd_touch_handle_t touch = nullptr;
    if (esp_lcd_touch_new_i2c_spd2010(touch_io, &touch_config, &touch) !=
        ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 touch driver");
        return false;
    }
    // Prime the controller, matching the factory bring-up order.
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(touch);
    vTaskDelay(pdMS_TO_TICKS(100));

    const lvgl_port_touch_cfg_t lvgl_touch = {
        .disp = display_,
        .handle = touch,
        .scale = {.x = 1.0f, .y = 1.0f},
    };
    touch_indev_ = lvgl_port_add_touch(&lvgl_touch);
    if (touch_indev_ == nullptr) {
        ESP_LOGE(kTag, "Cannot register SPD2010 touch with LVGL");
        return false;
    }
    // Sample the panel faster than the refresh period so drags track the
    // finger instead of stepping once per rendered frame.
    if (lvgl_port_lock(100)) {
        lv_lock();
        lv_timer_set_period(lv_indev_get_read_timer(touch_indev_), kTouchPollMs);
        lv_unlock();
        lvgl_port_unlock();
    }
    ESP_LOGI(kTag, "SPD2010 touch initialized (poll %d ms)", kTouchPollMs);
    return true;
}

esp_err_t SensecapWatcherHardware::TouchPanelTxParam(esp_lcd_panel_io_t* io,
                                                     int lcd_cmd,
                                                     const void* param,
                                                     size_t param_size) {
    auto* self = reinterpret_cast<TouchPanelIo*>(io);
    if (self == nullptr || self->device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lcd_cmd < 0 || param == nullptr || param_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // The SPD2010 command protocol carries everything in the data phase.
    return i2c_master_transmit(self->device,
                               static_cast<const uint8_t*>(param), param_size,
                               kTouchI2cTimeoutMs) == ESP_OK
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t SensecapWatcherHardware::TouchPanelRxParam(esp_lcd_panel_io_t* io,
                                                     int lcd_cmd, void* param,
                                                     size_t param_size) {
    auto* self = reinterpret_cast<TouchPanelIo*>(io);
    if (self == nullptr || self->device == nullptr || param == nullptr ||
        param_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)lcd_cmd;
    return i2c_master_receive(self->device, static_cast<uint8_t*>(param),
                              param_size,
                              kTouchI2cTimeoutMs) == ESP_OK
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t SensecapWatcherHardware::TouchPanelTxColor(esp_lcd_panel_io_t*,
                                                     int, const void*,
                                                     size_t) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t SensecapWatcherHardware::TouchPanelDel(esp_lcd_panel_io_t*) {
    return ESP_OK;
}

esp_err_t SensecapWatcherHardware::TouchPanelRegisterCallbacks(
    esp_lcd_panel_io_t*, const esp_lcd_panel_io_callbacks_t*, void*) {
    return ESP_ERR_NOT_SUPPORTED;
}

bool SensecapWatcherHardware::InitializeBattery() {
    const adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = WATCHER_BATTERY_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&adc_config, &adc_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create battery ADC unit");
        return false;
    }
    return battery_.Initialize(&io_expander_, adc_);
}

bool SensecapWatcherHardware::InitializeInputs() {
    // Wheel rotation controls the system volume.
    const knob_config_t knob_config = {
        .default_direction = 0,
        .gpio_encoder_a = static_cast<uint8_t>(WATCHER_KNOB_A),
        .gpio_encoder_b = static_cast<uint8_t>(WATCHER_KNOB_B),
    };
    knob_ = iot_knob_create(&knob_config);
    if (knob_ == nullptr) {
        ESP_LOGE(kTag, "Cannot create wheel encoder");
        return false;
    }
    iot_knob_register_cb(knob_, KNOB_RIGHT, OnKnobRotateUp, this);
    iot_knob_register_cb(knob_, KNOB_LEFT, OnKnobRotateDown, this);

    // The wheel button doubles as the power key.
    auto* driver = static_cast<PowerKeyDriver*>(
        calloc(1, sizeof(PowerKeyDriver)));
    if (driver == nullptr) return false;
    driver->base.enable_power_save = false;
    driver->base.get_key_level = ReadPowerKeyLevel;
    driver->base.del = DeletePowerKeyDriver;
    driver->hardware = this;
    const button_config_t button_config = {
        .long_press_time = WATCHER_POWER_KEY_LONG_PRESS_MS,
        .short_press_time = WATCHER_POWER_KEY_SHORT_PRESS_MS,
    };
    if (iot_button_create(&button_config, &driver->base, &power_key_) !=
        ESP_OK) {
        ESP_LOGE(kTag, "Cannot create power key");
        return false;
    }
    iot_button_register_cb(power_key_, BUTTON_PRESS_DOWN, nullptr,
                           OnPowerKeyPressDown, this);
    iot_button_register_cb(power_key_, BUTTON_SINGLE_CLICK, nullptr,
                           OnPowerKeyClick, this);
    iot_button_register_cb(power_key_, BUTTON_DOUBLE_CLICK, nullptr,
                           OnPowerKeyDoubleClick, this);
    iot_button_register_cb(power_key_, BUTTON_LONG_PRESS_START, nullptr,
                           OnPowerKeyLongPress, this);
    return true;
}

bool SensecapWatcherHardware::PollPowerKey(bool* pressed) {
    if (pressed == nullptr) return false;
    bool level = false;
    if (!io_expander_.InputLevel(WATCHER_IO_KNOB_BTN, &level)) return false;
    *pressed = !level;
    return true;
}

void SensecapWatcherHardware::AttachSystem(
    pxsys_standard_system_t* system, pxsys_reference_lvgl_t* reference_ui) {
    system_ = system;
    reference_ui_ = reference_ui;
    PublishStatus();
}

void SensecapWatcherHardware::SetBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    brightness_.store(percent);
    const uint32_t duty = screen_enabled_.load()
                              ? static_cast<uint32_t>(percent) * 1023 / 100
                              : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL);
    ScheduleStatusUpdate();
}

void SensecapWatcherHardware::SetVolume(uint8_t percent) {
    audio_.SetVolume(percent);
    ScheduleStatusUpdate();
}

void SensecapWatcherHardware::PublishStatus() {
    if (system_ == nullptr) return;
    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    uint8_t battery_percent = 0;
    if (battery_.GetPercent(&battery_percent)) {
        status.battery_valid = 1;
        status.battery_percent = battery_percent;
        status.charging = battery_.IsCharging() ? 1 : 0;
    }
    const bool wifi_initialized = wifi_initialized_.load();
    const bool wifi_enabled = wifi_initialized && wifi_enabled_.load();
    auto& wifi = WifiManager::GetInstance();
    const bool wifi_connected = wifi_initialized && wifi.IsConnected();
    status.network_connected = wifi_connected;
    status.network_type =
        wifi_connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    status.network_signal_level =
        wifi_connected ? SignalLevel(wifi.GetRssi()) : 0;
    status.wifi_supported = wifi_initialized ? 1 : 0;
    status.wifi_enabled = wifi_enabled ? 1 : 0;
    status.wifi_connected = wifi_connected ? 1 : 0;
    status.wifi_signal_level = status.network_signal_level;
    status.volume_supported = 1;
    status.volume_percent = audio_.volume();
    status.brightness_supported = 1;
    status.brightness_percent = brightness_.load();
    (void)pxsys_system_status_service_update(
        pxsys_standard_system_status(system_), &status);
}

bool SensecapWatcherHardware::InitializeWifi() {
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected || event == WifiEvent::Disconnected ||
            event == WifiEvent::ConfigModeEnter ||
            event == WifiEvent::ConfigModeExit) {
            ScheduleStatusUpdate();
        }
    });
    WifiManagerConfig config;
    config.ssid_prefix = "SenseCAP-Watcher";
    config.language = "zh-CN";
    if (!wifi.Initialize(config)) return false;
    wifi_initialized_.store(true);
    wifi.StartStation();
    return true;
}

void SensecapWatcherHardware::SetWifiEnabled(bool enabled) {
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

void SensecapWatcherHardware::EnterWifiProvisioning() {
    if (!wifi_initialized_.load()) return;
    wifi_enabled_.store(true);
    WifiManager::GetInstance().StartConfigAp();
    ESP_LOGI(kTag, "Wi-Fi provisioning AP requested (SenseCAP-Watcher-xxxx)");
}

void SensecapWatcherHardware::ShowInitialFrame() {
    if (display_ == nullptr) return;
    if (lvgl_port_lock(1000)) {
        lv_lock();
        lv_refr_now(display_);
        lv_unlock();
        lvgl_port_unlock();
    }
    SetBrightness(kDefaultBrightness);
}

void SensecapWatcherHardware::ScheduleStatusUpdate() {
    if (system_ == nullptr || status_update_pending_.exchange(true)) return;
    lv_lock();
    lv_async_call(StatusOnLvgl, this);
    lv_unlock();
}

void SensecapWatcherHardware::StatusTimer(void* context) {
    static_cast<SensecapWatcherHardware*>(context)->ScheduleStatusUpdate();
}

void SensecapWatcherHardware::StatusOnLvgl(void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    self->status_update_pending_.store(false);
    self->PublishStatus();
}

void SensecapWatcherHardware::OnKnobRotateUp(void*, void* context) {
    static_cast<SensecapWatcherHardware*>(context)->AdjustVolume(kVolumeStep);
}

void SensecapWatcherHardware::OnKnobRotateDown(void*, void* context) {
    static_cast<SensecapWatcherHardware*>(context)->AdjustVolume(-kVolumeStep);
}

void SensecapWatcherHardware::AdjustVolume(int delta) {
    if (delta == 0) return;
    if (pxa_host_captures_volume_keys()) {
        const pxa_host_key_t key =
            delta > 0 ? PXA_HOST_KEY_VOLUME_UP : PXA_HOST_KEY_VOLUME_DOWN;
        if (pxa_host_post_key(key)) return;
    }
    const int target = std::min(100, std::max(0, audio_.volume() + delta));
    SetVolume(static_cast<uint8_t>(target));
}

void SensecapWatcherHardware::OnPowerKeyPressDown(void*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    self->power_key_woke_screen_.store(!self->screen_enabled_.load());
    self->WakeScreen();
}

void SensecapWatcherHardware::OnPowerKeyClick(void*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    // A press that only turned the screen back on must not toggle it off.
    if (self->power_key_woke_screen_.exchange(false)) return;
    self->ToggleScreen();
}

void SensecapWatcherHardware::OnPowerKeyDoubleClick(void*, void* context) {
    static_cast<SensecapWatcherHardware*>(context)->EnterWifiProvisioning();
}

void SensecapWatcherHardware::OnPowerKeyLongPress(void*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    // Ignore the press that was used to power the device on.
    if (esp_timer_get_time() - self->boot_time_us_ < kPowerKeyBootGraceUs) {
        return;
    }
    if (xTaskCreate(
            [](void* arg) {
                static_cast<SensecapWatcherHardware*>(arg)->PowerOff();
            },
            "watcher_power_off", 3072, self, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Cannot create power-off task");
    }
}

void SensecapWatcherHardware::WakeScreen() {
    if (screen_enabled_.load()) return;
    screen_enabled_.store(true);
    SetBrightness(brightness_.load() == 0 ? kDefaultBrightness
                                          : brightness_.load());
}

void SensecapWatcherHardware::ToggleScreen() {
    const bool enabled = !screen_enabled_.load();
    screen_enabled_.store(enabled);
    if (enabled) {
        SetBrightness(brightness_.load() == 0 ? kDefaultBrightness
                                              : brightness_.load());
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL);
    }
}

void SensecapWatcherHardware::PowerOff() {
    ESP_LOGI(kTag, "Releasing the power latch");
    SetBrightness(0);
    io_expander_.SetOutputs(WATCHER_IO_PWR_SYSTEM, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // With USB attached the rails stay up; restart instead of hanging dark.
    esp_restart();
}
