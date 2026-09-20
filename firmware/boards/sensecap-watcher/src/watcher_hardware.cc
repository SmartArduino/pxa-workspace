#include "watcher_hardware.h"

#include "sensecap_watcher_config.h"
#include "watcher_pxa_surface.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_heap_caps.h>
#include <esp_lcd_spd2010.h>
#include <esp_lcd_touch.h>
#include "watcher_spd2010_touch.h"
#include <esp_log.h>
#include <esp_lv_decoder.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_knob.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>
#include <pxsys/system_status.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "WatcherHw";
// The SPI driver bounces PSRAM color data through internal DMA buffers that
// are allocated per transaction. Keep the chunks small (8 KiB, two in flight)
// so those allocations cannot fail under internal-RAM pressure; a failed
// transfer would otherwise leave LVGL waiting for a completion forever.
constexpr int kLcdChunkBytes = 8 * 1024;
constexpr int kLcdQueueDepth = 2;
// The flush task only runs the SPI submission and its bounce copies.
constexpr int kFlushTaskPriority = 4;
constexpr uint32_t kFlushTaskStack = 4096;
// The official BSP polls the touchpad on the LVGL indev timer and treats a
// report as a touch only when its pressure is above this threshold, which
// also filters the controller's zero-pressure lift records.
constexpr int kTouchPollMs = 6;
constexpr unsigned kTouchSensitivity = 20;
// A live finger makes the controller report continuously (heartbeat reports
// every ~16 ms even when it does not move), so a short silence means the lift.
// The controller's explicit lift record releases instantly; this timeout only
// bounds a lost report.
constexpr int64_t kTouchReleaseTimeoutUs = 300 * 1000;
// A pointer slot survives this long without appearing in a report, so a
// report that only carries the changed fingers does not release the others.
constexpr int64_t kTouchSlotGraceUs = 120 * 1000;
// Safety net in case the expander interrupt line is not connected: keep the
// touch task sampling on this cadence anyway.
constexpr int kTouchFallbackPollMs = 50;
constexpr int kTouchTaskPriority = 10;
constexpr uint32_t kTouchTaskStack = 3072;
constexpr int64_t kTouchLogIntervalUs = 150 * 1000;
constexpr int kTouchI2cTimeoutMs = 100;
constexpr int kVolumeStep = 5;
constexpr uint8_t kDefaultBrightness = 75;
constexpr int64_t kPowerKeyBootGraceUs = 3 * 1000 * 1000;
// Periodic memory report: SRAM and PSRAM usage including the lowest free
// value seen and the largest allocatable block.
constexpr int64_t kHeapLogIntervalUs = 10 * 1000 * 1000;

uint8_t SignalLevel(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

#if LV_USE_LOG
// Route the LVGL log module (the performance monitor prints through it) to
// the ESP log so the lines carry the usual tag and timestamp.
void LvglLogCallback(lv_log_level_t level, const char* buffer) {
    esp_log_level_t esp_level = ESP_LOG_INFO;
    switch (level) {
        case LV_LOG_LEVEL_TRACE:
            esp_level = ESP_LOG_VERBOSE;
            break;
        case LV_LOG_LEVEL_INFO:
        case LV_LOG_LEVEL_USER:
            esp_level = ESP_LOG_INFO;
            break;
        case LV_LOG_LEVEL_WARN:
            esp_level = ESP_LOG_WARN;
            break;
        case LV_LOG_LEVEL_ERROR:
            esp_level = ESP_LOG_ERROR;
            break;
        default:
            return;
    }
    esp_log_write(esp_level, "LVGL", "%s", buffer);
}
#endif

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
    ESP_LOGI(kTag,
             "Heap after bring-up: internal %u/%u free (min %u), PSRAM %u/%u free",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
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
        .max_transfer_sz = kLcdChunkBytes,
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
#if LV_USE_LOG
    lv_log_register_print_cb(LvglLogCallback);
#endif
    // Register the optimized PNG/JPEG decoders before any image is loaded.
    if (esp_lv_decoder_init(&image_decoder_) != ESP_OK) {
        image_decoder_ = nullptr;
        ESP_LOGW(kTag, "Continuing with the built-in image decoders");
    }
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
        // Full-screen PSRAM draw buffer with two software draw units: LVGL
        // splits the refreshed area into two tiles and renders them on both
        // cores, without the per-band tiling overhead internal RAM bands
        // introduced. The native swapped format removes the CPU byte swap.
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
            .direct_mode = false,
        },
    };
    display_ = lvgl_port_add_disp(&display_config);
    if (display_ == nullptr) {
        ESP_LOGE(kTag, "Cannot register SPD2010 display with LVGL");
        return false;
    }
    // Submitting a PSRAM frame costs the SPI driver a bounce copy per chunk.
    // A dedicated task does that work so the LVGL task can start the next
    // render immediately; LVGL still waits for the transfer completion.
    flush_queue_ = xQueueCreate(2, sizeof(FlushRequest));
    if (flush_queue_ == nullptr ||
        xTaskCreate(FlushTaskEntry, "watcher_flush", kFlushTaskStack, this,
                    kFlushTaskPriority, &flush_task_) != pdPASS) {
        ESP_LOGE(kTag, "Cannot start the display flush task");
        return false;
    }
    // The port's flush would leave LVGL spinning forever if the panel IO ever
    // fails to queue a transfer (LVGL waits on a completion callback that
    // only fires on success), so the board installs a flush that reports the
    // error and always releases the display.
    if (lvgl_port_lock(100)) {
        lv_display_set_user_data(display_, this);
        lv_display_set_flush_cb(display_, FlushDisplay);
        lvgl_port_unlock();
    }
    if (!watcher_pxa_surface::Install(display_)) {
        ESP_LOGW(kTag, "Continuing without PXA Surface presentation");
    }
    ESP_LOGI(kTag, "SPD2010 display initialized as %dx%d",
             WATCHER_DISPLAY_WIDTH, WATCHER_DISPLAY_HEIGHT);
    return true;
}

void SensecapWatcherHardware::FlushDisplay(lv_display_t* display,
                                           const lv_area_t* area,
                                           uint8_t* pixels) {
    auto* self = static_cast<SensecapWatcherHardware*>(
        lv_display_get_user_data(display));
    if (self == nullptr || self->panel_ == nullptr ||
        self->flush_queue_ == nullptr) {
        lv_display_flush_ready(display);
        return;
    }
    // Composite the latest PXA Surface under the trusted UI before the area
    // leaves the LVGL task; the queued pixels are then fully owned by the
    // flush task and the Surface lease can be released immediately.
    watcher_pxa_surface::ComposeFlushArea(area, pixels);
    if (lv_display_flush_is_last(display)) pxa_board_performance_note_frame();
    pxa_board_performance_draw_rgb565(
        reinterpret_cast<uint16_t*>(pixels), WATCHER_DISPLAY_WIDTH,
        WATCHER_DISPLAY_HEIGHT, static_cast<uint32_t>(lv_area_get_width(area)),
        area->x1, area->y1, static_cast<uint16_t>(lv_area_get_width(area)),
        static_cast<uint16_t>(lv_area_get_height(area)), true);
    const FlushRequest request = {
        .display = display,
        .area = *area,
        .pixels = pixels,
    };
    if (xQueueSend(self->flush_queue_, &request, 0) != pdTRUE) {
        // LVGL waits for the previous transfer before the next flush, so the
        // queue should never fill; drop the frame instead of blocking the
        // render task if it ever does.
        ESP_LOGW(kTag, "flush queue full, dropping frame");
        lv_display_flush_ready(display);
    }
}

void SensecapWatcherHardware::FlushTaskEntry(void* arg) {
    static_cast<SensecapWatcherHardware*>(arg)->FlushTask();
}

void SensecapWatcherHardware::FlushTask() {
    FlushRequest request = {};
    for (;;) {
        if (xQueueReceive(flush_queue_, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const esp_err_t err = esp_lcd_panel_draw_bitmap(
            panel_, request.area.x1, request.area.y1, request.area.x2 + 1,
            request.area.y2 + 1, request.pixels);
        if (err != ESP_OK) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - flush_error_us_ > 1000 * 1000) {
                flush_error_us_ = now_us;
                ESP_LOGE(kTag, "flush failed (%s): %dx%d at %d,%d",
                         esp_err_to_name(err),
                         (int)(request.area.x2 - request.area.x1 + 1),
                         (int)(request.area.y2 - request.area.y1 + 1),
                         (int)request.area.x1, (int)request.area.y1);
            }
            // Nothing will complete this transfer, so do not leave LVGL
            // waiting; the success path is released by the panel IO callback.
            lv_display_flush_ready(request.display);
        }
    }
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
    if (watcher_spd2010_touch_new(touch_io, &touch_config, &touch) !=
        ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 touch driver");
        return false;
    }
    // Prime the controller the way the official BSP does before adding the
    // input device.
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(touch);
    vTaskDelay(pdMS_TO_TICKS(100));
    touch_ = touch;
    // The PCA9555 asserts its interrupt (GPIO2) whenever an input changes,
    // and the SPD2010 raises P0.5 when a report is ready. Wake a dedicated
    // task on that edge so the controller is read immediately instead of on
    // a slow poll.
    touch_sem_ = xSemaphoreCreateBinary();
    if (touch_sem_ == nullptr) {
        ESP_LOGE(kTag, "Cannot create touch interrupt semaphore");
        return false;
    }
    const gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << WATCHER_IO_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    if (gpio_config(&int_config) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot configure the expander interrupt pin");
        return false;
    }
    const esp_err_t isr_service = gpio_install_isr_service(0);
    if (isr_service != ESP_OK && isr_service != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Cannot install the GPIO ISR service");
        return false;
    }
    if (gpio_isr_handler_add(WATCHER_IO_INT, TouchInterruptHandler, this) !=
        ESP_OK) {
        ESP_LOGE(kTag, "Cannot attach the expander interrupt handler");
        return false;
    }
    if (xTaskCreate(TouchTaskEntry, "watcher_touch", kTouchTaskStack, this,
                    kTouchTaskPriority, &touch_task_) != pdPASS) {
        ESP_LOGE(kTag, "Cannot start the touch task");
        return false;
    }
    // One pointer indev per touch slot: LVGL and the PXA Guest bridge see
    // every finger, and the Guest bridge derives a stable pointer id from the
    // indev order. The read callback only publishes the state the touch task
    // already fetched, so the fast timers stay cheap.
    if (lvgl_port_lock(100)) {
        for (int i = 0; i < kTouchMaxPointers; ++i) {
            lv_indev_t* indev = lv_indev_create();
            if (indev == nullptr) continue;
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, TouchReadCallback);
            lv_indev_set_disp(indev, display_);
            lv_indev_set_driver_data(indev, this);
            lv_indev_set_user_data(
                indev, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            lv_timer_set_period(lv_indev_get_read_timer(indev), kTouchPollMs);
            if (touch_indev_ == nullptr) touch_indev_ = indev;
        }
        lvgl_port_unlock();
    }
    if (touch_indev_ == nullptr) {
        ESP_LOGE(kTag, "Cannot register SPD2010 touch with LVGL");
        return false;
    }
    ESP_LOGI(kTag, "SPD2010 touch initialized (%d pointers, poll %d ms)",
             kTouchMaxPointers, kTouchPollMs);
    return true;
}

void SensecapWatcherHardware::TouchReadCallback(lv_indev_t* indev,
                                                lv_indev_data_t* data) {
    auto* self = static_cast<SensecapWatcherHardware*>(
        lv_indev_get_driver_data(indev));
    if (self == nullptr || self->touch_ == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    const int slot = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_indev_get_user_data(indev)));
    if (slot < 0 || slot >= kTouchMaxPointers) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Publish the state the touch task already fetched; doing I2C here would
    // make the fast timer expensive and would delay the touch task.
    portENTER_CRITICAL(&self->touch_lock_);
    data->point = self->touch_slot_point_[slot];
    data->state = self->touch_slot_pressed_[slot] ? LV_INDEV_STATE_PRESSED
                                                  : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&self->touch_lock_);
}

void IRAM_ATTR SensecapWatcherHardware::TouchInterruptHandler(void* arg) {
    auto* self = static_cast<SensecapWatcherHardware*>(arg);
    BaseType_t woken = pdFALSE;
    if (self != nullptr && self->touch_sem_ != nullptr) {
        xSemaphoreGiveFromISR(self->touch_sem_, &woken);
    }
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

void SensecapWatcherHardware::TouchTaskEntry(void* arg) {
    static_cast<SensecapWatcherHardware*>(arg)->TouchTask();
}

void SensecapWatcherHardware::TouchTask() {
    for (;;) {
        // Wake on the expander interrupt (any input change) with a slow
        // fallback so the touch keeps working if that line is missing.
        xSemaphoreTake(touch_sem_, pdMS_TO_TICKS(kTouchFallbackPollMs));
        // Reading the input register clears the PCA9555 interrupt latch, which
        // re-arms the pin for the next change.
        uint16_t inputs = 0;
        io_expander_.ReadInputs(&inputs);
        PollTouchController();
    }
}

void SensecapWatcherHardware::PollTouchController() {
    // The vendored driver exposes the controller's own view of the touch:
    // `pressed` is set by reports that carry pressure and cleared only by the
    // controller's lift record. Empty samples therefore hold a press instead
    // of ending it, which the stock driver's point data cannot distinguish.
    esp_lcd_touch_point_data_t points[kTouchMaxPointers] = {};
    uint8_t count = 0;
    const int64_t now_us = esp_timer_get_time();
    const esp_err_t err = esp_lcd_touch_read_data(touch_);
    if (err == ESP_OK) {
        (void)esp_lcd_touch_get_data(touch_, points, &count,
                                     kTouchMaxPointers);
    } else if (now_us - touch_log_us_ >= kTouchLogIntervalUs) {
        touch_log_us_ = now_us;
        ESP_LOGW(kTag, "touch read failed: %s", esp_err_to_name(err));
    }
    // Log the strongest point; it is the one the user is most likely driving.
    uint8_t strongest = 0;
    for (uint8_t i = 1; i < count; ++i) {
        if (points[i].strength > points[strongest].strength) strongest = i;
    }
    if (count > 0) {
        LogTouch(count, points[strongest].x, points[strongest].y,
                 points[strongest].strength);
    }

    const char* release_reason = nullptr;
    portENTER_CRITICAL(&touch_lock_);
    const bool lift = !watcher_spd2010_touch_pressed();
    const bool lost =
        touch_pressed_ && now_us - touch_active_us_ > kTouchReleaseTimeoutUs;
    if (lift || lost) {
        if (touch_pressed_) {
            touch_pressed_ = false;
            release_reason = lift ? "lift record" : "no reports";
        }
        for (int i = 0; i < kTouchMaxPointers; ++i) {
            touch_slot_pressed_[i] = false;
        }
    } else if (count > 0) {
        const bool strong = strongest < count &&
                            points[strongest].strength > kTouchSensitivity;
        if (strong) {
            // Map the report onto the pointer slots, keeping a finger on the
            // same slot (and therefore the same LVGL indev and Guest pointer
            // id) by its controller track id.
            bool slot_taken[kTouchMaxPointers] = {};
            for (uint8_t i = 0; i < count; ++i) {
                if (points[i].strength == 0) continue;
                int slot = -1;
                for (int s = 0; s < kTouchMaxPointers; ++s) {
                    if (!slot_taken[s] && touch_slot_pressed_[s] &&
                        touch_slot_id_[s] == points[i].track_id) {
                        slot = s;
                        break;
                    }
                }
                for (int s = 0; slot < 0 && s < kTouchMaxPointers; ++s) {
                    if (!slot_taken[s] &&
                        (!touch_slot_pressed_[s] ||
                         now_us - touch_slot_seen_us_[s] > kTouchSlotGraceUs)) {
                        slot = s;
                    }
                }
                if (slot < 0) continue;
                slot_taken[slot] = true;
                touch_slot_pressed_[slot] = true;
                touch_slot_id_[slot] = points[i].track_id;
                touch_slot_point_[slot].x = points[i].x;
                touch_slot_point_[slot].y = points[i].y;
                touch_slot_seen_us_[slot] = now_us;
            }
            touch_pressed_ = true;
            touch_active_us_ = now_us;
        }
        // Slots missing from the report are released after a short grace, so
        // a report that only carries the changed fingers does not flicker.
        for (int s = 0; s < kTouchMaxPointers; ++s) {
            if (touch_slot_pressed_[s] &&
                now_us - touch_slot_seen_us_[s] > kTouchSlotGraceUs) {
                touch_slot_pressed_[s] = false;
            }
        }
    }
    portEXIT_CRITICAL(&touch_lock_);
    if (release_reason != nullptr) {
        ESP_LOGI(kTag, "touch: released (%s)", release_reason);
    }
}

void SensecapWatcherHardware::LogTouch(uint8_t count, int32_t x, int32_t y,
                                       uint16_t strength) {
    const int64_t now_us = esp_timer_get_time();
    if (count == touch_log_count_ &&
        now_us - touch_log_us_ < kTouchLogIntervalUs) {
        return;
    }
    touch_log_us_ = now_us;
    touch_log_count_ = count;
    if (count == 0) {
        ESP_LOGD(kTag, "touch: no points");
    } else {
        ESP_LOGD(kTag, "touch: 1 finger (%d,%d) w=%u", static_cast<int>(x),
                 static_cast<int>(y), static_cast<unsigned>(strength));
    }
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
    iot_button_register_cb(power_key_, BUTTON_PRESS_UP, nullptr,
                           OnPowerKeyPressUp, this);
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
    if (reference_ui_ != nullptr) {
        (void)pxsys_reference_lvgl_set_lock_changed_callback(
            reference_ui_, this,
            [](void* context, bool locked) {
                static_cast<SensecapWatcherHardware*>(context)
                    ->OnLockChanged(locked);
            });
        (void)pxsys_reference_lvgl_set_power_action_callback(
            reference_ui_, this,
            [](void* context, pxsys_reference_power_action_t action) {
                auto* self = static_cast<SensecapWatcherHardware*>(context);
                if (action == PXSYS_REFERENCE_POWER_ACTION_SHUTDOWN) {
                    if (xTaskCreate(
                            [](void* arg) {
                                static_cast<SensecapWatcherHardware*>(arg)
                                    ->PowerOff();
                            },
                            "watcher_power_off", 3072, self, 4, nullptr) !=
                        pdPASS) {
                        ESP_LOGE(kTag, "Cannot create power-off task");
                    }
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
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    self->LogHeapUsage();
    self->ScheduleStatusUpdate();
}

void SensecapWatcherHardware::LogHeapUsage() {
    const int64_t now_us = esp_timer_get_time();
    if (now_us - heap_log_us_ < kHeapLogIntervalUs) return;
    heap_log_us_ = now_us;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_total = heap_caps_get_total_size(internal_caps);
    const size_t internal_free = heap_caps_get_free_size(internal_caps);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(kTag,
             "Heap: SRAM used=%u free=%u total=%u min_free=%u largest=%u "
             "| PSRAM used=%u free=%u total=%u min_free=%u largest=%u",
             static_cast<unsigned>(internal_total - internal_free),
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(internal_total),
             static_cast<unsigned>(
                 heap_caps_get_minimum_free_size(internal_caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(internal_caps)),
             static_cast<unsigned>(psram_total - psram_free),
             static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_total),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
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
    self->power_key_long_press_.store(false);
    self->power_key_woke_screen_.store(!self->screen_enabled_.load());
    self->WakeScreen();
}

void SensecapWatcherHardware::OnPowerKeyPressUp(void*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    if (self->power_key_long_press_.load())
        self->power_key_woke_screen_.store(false);
}

void SensecapWatcherHardware::OnPowerKeyClick(void*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    if (self->power_key_long_press_.exchange(false)) return;
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
    if (self->power_key_woke_screen_.load()) return;
    self->power_key_long_press_.store(true);
    lv_lock();
    const lv_result_t result = lv_async_call(
        [](void* arg) {
            auto* hardware = static_cast<SensecapWatcherHardware*>(arg);
            if (hardware->reference_ui_ != nullptr)
                (void)pxsys_reference_lvgl_show_power_menu(
                    hardware->reference_ui_);
        },
        self);
    lv_unlock();
    if (result != LV_RESULT_OK)
        ESP_LOGE(kTag, "Cannot schedule power menu");
}

void SensecapWatcherHardware::WakeScreen() {
    SetScreenEnabled(true);
}

void SensecapWatcherHardware::SetScreenEnabled(bool enabled) {
    const bool previous = screen_enabled_.exchange(enabled);
    if (previous == enabled) return;
    pxa_esp_surface_set_host_visible(false);
    if (!enabled) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL);
    }
    lv_lock();
    const lv_result_t result = lv_async_call(
        [](void* context) {
            auto* self = static_cast<SensecapWatcherHardware*>(context);
            if (self->reference_ui_ != nullptr)
                (void)pxsys_reference_lvgl_set_locked(self->reference_ui_, true);
            if (!self->screen_enabled_.load()) return;
            if (self->display_ != nullptr) lv_refr_now(self->display_);
            self->SetBrightness(self->brightness_.load() == 0
                                    ? kDefaultBrightness
                                    : self->brightness_.load());
        },
        this);
    lv_unlock();
    if (result != LV_RESULT_OK)
        ESP_LOGE(kTag, "Cannot schedule screen %s",
                 enabled ? "wake" : "lock");
}

void SensecapWatcherHardware::ToggleScreen() {
    SetScreenEnabled(!screen_enabled_.load());
}

void SensecapWatcherHardware::OnLockChanged(bool locked) {
    pxa_esp_surface_set_host_visible(!locked);
}

void SensecapWatcherHardware::PowerOff() {
    ESP_LOGI(kTag, "Releasing the power latch");
    SetBrightness(0);
    io_expander_.SetOutputs(WATCHER_IO_PWR_SYSTEM, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // With USB attached the rails stay up; restart instead of hanging dark.
    esp_restart();
}
