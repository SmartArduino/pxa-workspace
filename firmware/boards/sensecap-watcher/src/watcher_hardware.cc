#include "watcher_hardware.h"

#include "sensecap_watcher_config.h"
#include "watcher_pxa_surface.h"
#include "watcher_lcd_io.h"

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
#include <esp_netif_sntp.h>
#include <esp_lv_decoder.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_knob.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <pxa_board_common/display_completion.h>
#include <pxa_board_common/rgb_port.h>
#include <pxa_board_common/i2c_port.h>
#include <pxsys/reference_lvgl.h>
#include <pxsys/standard_system.h>
#include <pxsys/system_status.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "WatcherHw";
// One LVGL draw unit leaves room for audio and the fixed DMA staging blocks.
// The bus limit also bounds the dynamic fallback used if staging is unavailable.
constexpr int kLcdQueueDepth = 2;
// Twelve full rows bound the two reserved DMA blocks to 19776 bytes. Packing
// directly into SRAM removes both hot-path allocation and a PSRAM bounce.
constexpr size_t kScanoutRows = 12;
constexpr size_t kScanoutBlockBytes = WATCHER_DISPLAY_WIDTH * kScanoutRows * 2;
constexpr int kLcdChunkBytes = kScanoutBlockBytes;
static_assert(kScanoutBlockBytes % 4 == 0);
// esp_lcd uses an exclusive x_end; both 0 and 412 meet SPD2010's rule.
static_assert(WATCHER_DISPLAY_WIDTH % 4 == 0);

void PackNativeFrame(void* timing, const void* source, void* destination,
                     size_t offset, size_t bytes) {
    const int64_t start = timing ? esp_timer_get_time() : 0;
    // Raw pixels are already validated/leased by PresentDirectFrame. Do not
    // manufacture a Surface descriptor: a default visible=false silently
    // skipped composition and left old scratch pixels (including FPS) intact.
    watcher_pxa_surface::PackNativeRgb565Band(source, destination, offset, bytes);
    const int y = offset / (WATCHER_DISPLAY_WIDTH * 2);
    const int rows = bytes / (WATCHER_DISPLAY_WIDTH * 2);
    pxa_board_performance_draw_rgb565(static_cast<uint16_t*>(destination),
        WATCHER_DISPLAY_WIDTH, WATCHER_DISPLAY_HEIGHT, WATCHER_DISPLAY_WIDTH,
        0, y, WATCHER_DISPLAY_WIDTH, rows, true);
    if (timing)
        *static_cast<uint64_t*>(timing) += esp_timer_get_time() - start;
}
// The existing flush task packs/stages pixels and owns panel submission.
constexpr int kFlushTaskPriority = 4;
// Measured about 2 KiB with UI/game/capture; retain about 1 KiB headroom.
constexpr uint32_t kFlushTaskStack = 3072;
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

void WatcherInitializeRtc(i2c_master_bus_handle_t bus);
bool WatcherInitializeVision();

bool SensecapWatcherHardware::Initialize() {
    boot_time_us_ = esp_timer_get_time();
    if (!InitializeI2c()) return false;
    if (!InitializePower()) return false;
    if (!InitializeRgbLed()) ESP_LOGW(kTag, "Continuing with the RGB LED on");
    // J5 Grove shares I2C0 with internal devices. Expose transactions only at
    // external addresses; never let an app reconfigure pins, rails or codecs.
    const uint8_t internal_addresses[] = {0x13, 0x14, 0x18, WATCHER_IO_EXPANDER_ADDRESS, 0x30, 0x51};
    if (!pxa_board_i2c_port_initialize(i2c_bus_, 2, "grove.i2c", internal_addresses, sizeof(internal_addresses)))
        ESP_LOGW(kTag, "Grove application interface unavailable");
    if (!InitializeDisplay()) return false;
    if (!lvgl_port_lock(1000)) return false;
    lv_lock();
    boot_screen_.Show(display_);
    lv_unlock();
    lvgl_port_unlock();
    ShowInitialFrame();
    if (InitializeSharedSpi()) {
        if (!InitializeSd()) ESP_LOGW(kTag, "Continuing without SD storage");
        if (!WatcherInitializeVision()) ESP_LOGW(kTag, "Continuing without vision extension");
    } else ESP_LOGW(kTag, "Continuing without SD and vision");
    WatcherInitializeRtc(i2c_bus_);
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
    return pxa_board_rgb_port_initialize(rgb_led_, [](void* context, unsigned r, unsigned g, unsigned b) {
        auto strip = static_cast<led_strip_handle_t>(context);
        return led_strip_set_pixel(strip, 0, r, g, b) == ESP_OK &&
               led_strip_refresh(strip) == ESP_OK;
    });
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
    backlight_.Configure(WATCHER_LCD_BACKLIGHT_CHANNEL,
                         (1u << WATCHER_LCD_BACKLIGHT_DUTY_RES) - 1u);

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
    if (WatcherLcdIoCreate(WATCHER_LCD_SPI_HOST, io_config,
                                 &panel_io_) != ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 panel IO");
        return false;
    }

    spd2010_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    /* Assignment rather than a full initializer: esp_lcd_panel_dev_config_t
     * reordered its fields between IDF releases. */
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
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
        // Full-width PSRAM buffers avoid narrow-band tiling overhead. One draw
        // unit keeps the fallback UI on its owner task without worker stacks.
        // Keep two approximately half-height UI buffers. Align their rows to
        // an eight-row allocation granularity, saving 9888 bytes at 412x412;
        // native games use their leased Surface rather than these buffers.
        .buffer_size = WATCHER_DISPLAY_WIDTH *
            ((WATCHER_DISPLAY_HEIGHT / 2) / 8 * 8),
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
    // PXADB screenshots are mirrored out of the flush path: the SPD2010 has no
    // readable framebuffer, so the flush callback copies the composed areas
    // into this snapshot before they are handed to the panel.
    snapshot_ = static_cast<uint16_t*>(heap_caps_aligned_alloc(64,
        static_cast<size_t>(WATCHER_DISPLAY_WIDTH) * WATCHER_DISPLAY_HEIGHT *
            sizeof(uint16_t),
        MALLOC_CAP_SPIRAM));
    snapshot_sem_ = xSemaphoreCreateBinary();
    if (snapshot_ == nullptr || snapshot_sem_ == nullptr) {
        ESP_LOGW(kTag, "Continuing without PXADB screen capture");
        if (snapshot_ != nullptr) {
            heap_caps_free(snapshot_);
            snapshot_ = nullptr;
        }
        if (snapshot_sem_ != nullptr) {
            vSemaphoreDelete(snapshot_sem_);
            snapshot_sem_ = nullptr;
        }
    }
    scanout_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        16, kScanoutBlockBytes * kLcdQueueDepth, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (scanout_ == nullptr)
        ESP_LOGW(kTag, "Continuing with bounded dynamic LCD DMA buffers");
#if CONFIG_PXADB_TEST_CONTROL
    // Native host tests cannot execute Xtensa PIE instructions. Verify the
    // actual device kernel once, using only this existing setup task's stack.
    alignas(16) uint16_t packing_source[96], packing_output[96];
    for (size_t i = 0; i < 96; ++i) packing_source[i] = (i * 173u) ^ 0xa55a;
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t count = 0; count < 80; ++count) {
            std::fill_n(packing_output, 96, 0x5a5a);
            watcher_pxa_surface::PackNativeRgb565Band(
                packing_source, packing_output + offset, offset * 2, count * 2);
            for (size_t i = 0; i < 96; ++i) {
                const uint16_t expected = i >= offset && i < offset + count
                    ? __builtin_bswap16(packing_source[i]) : 0x5a5a;
                if (packing_output[i] != expected) {
                    ESP_LOGE(kTag, "Native packing self-check failed");
                    return false;
                }
            }
        }
    }
    ESP_LOGI(kTag, "Native packing self-check passed: aligned, clipped and tail pixels");
#endif

    // The existing flush task owns packing/staging and DMA. LVGL or the game
    // presenter can prepare the next buffer while this owner transfers pixels.
    flush_queue_ = xQueueCreate(1, sizeof(FlushRequest));
    if (flush_queue_ == nullptr ||
        xTaskCreate(FlushTaskEntry, "watcher_flush", kFlushTaskStack, this,
                    kFlushTaskPriority, &flush_task_) != pdPASS) {
        ESP_LOGE(kTag, "Cannot start the display flush task");
        return false;
    }
    // Replace the port's flush-ready callback: completion telemetry and
    // screenshots must follow DMA, rather than the preceding composition.
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = OnFlushComplete,
    };
    // The port's flush would leave LVGL spinning forever if the panel IO ever
    // fails to queue a transfer (LVGL waits on a completion callback that
    // only fires on success), so the board installs a flush that reports the
    // error and always releases the display.
    if (!lvgl_port_lock(1000)) return false;
    // Drain a possible initial port flush while its original callback is
    // still installed. A command-less parameter transaction only waits for
    // queued SPI color transfers; it does not send a panel command.
    const esp_err_t drained = esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0);
    const esp_err_t registered = drained == ESP_OK
        ? esp_lcd_panel_io_register_event_callbacks(panel_io_, &callbacks, this)
        : drained;
    if (registered == ESP_OK) {
        lv_display_set_user_data(display_, this);
        lv_display_set_flush_cb(display_, FlushDisplay);
        lv_display_set_flush_wait_cb(display_, WaitFlushDisplay);
    }
    lvgl_port_unlock();
    if (registered != ESP_OK) return false;
    if (!watcher_pxa_surface::Install(display_, [](void* context) {
            return static_cast<SensecapWatcherHardware*>(context)->PresentDirectFrame();
        }, this)) {
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
    self->WaitDirectFrame();
    const auto composed = watcher_pxa_surface::ComposeFlushArea(area, pixels);
    self->direct_snapshot_current_ = false;
    const bool last = lv_display_flush_is_last(display);
    const uint16_t snapshot = self->snapshot_pending_.load(std::memory_order_acquire);
    if (snapshot != 0) {
        self->SnapshotFlushArea(area, pixels);
    }
    pxa_board_performance_draw_rgb565(
        reinterpret_cast<uint16_t*>(pixels), WATCHER_DISPLAY_WIDTH,
        WATCHER_DISPLAY_HEIGHT, static_cast<uint32_t>(lv_area_get_width(area)),
        area->x1, area->y1, static_cast<uint16_t>(lv_area_get_width(area)),
        static_cast<uint16_t>(lv_area_get_height(area)), true);
    const FlushRequest request = {
        .display = display,
        .area = *area,
        .pixels = pixels,
        .frame_id = composed.frame_id,
        .input_timestamp_us = composed.input_timestamp_us,
        .raster_ready_tick_us = composed.raster_ready_tick_us,
        .last = last,
        .snapshot_revision = snapshot,
    };
    self->ui_flush_done_.store(false, std::memory_order_release);
    if (xQueueSend(self->flush_queue_, &request, 0) != pdTRUE) {
        // LVGL waits for the previous transfer before the next flush, so the
        // queue should never fill; drop the frame instead of blocking the
        // render task if it ever does.
        ESP_LOGW(kTag, "flush queue full, dropping frame");
        self->ui_flush_done_.store(true, std::memory_order_release);
        lv_display_flush_ready(display);
    }
}

void SensecapWatcherHardware::WaitFlushDisplay(lv_display_t* display) {
    auto* self = static_cast<SensecapWatcherHardware*>(
        lv_display_get_user_data(display));
    if (self == nullptr) return;
    while (!self->ui_flush_done_.load(std::memory_order_acquire)) vTaskDelay(1);
    // LVGL clears its flushing flag after this callback. Only that owner
    // clears it: an asynchronous ready write could race the next UI flush.
}

void SensecapWatcherHardware::FlushTaskEntry(void* arg) {
    static_cast<SensecapWatcherHardware*>(arg)->FlushTask();
}

bool SensecapWatcherHardware::OnFlushComplete(
    esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context) {
    auto* self = static_cast<SensecapWatcherHardware*>(context);
    self->flush_done_us_low_.store(static_cast<uint32_t>(esp_timer_get_time()),
                                   std::memory_order_release);
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->flush_task_, &woken);
    return woken == pdTRUE;
}

void SensecapWatcherHardware::FlushTask() {
    FlushRequest request = {};
    for (;;) {
        if (xQueueReceive(flush_queue_, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        (void)ulTaskNotifyTake(pdTRUE, 0);
        const uint64_t lease = request.display == nullptr ? direct_lease_ : 0;
        const int64_t transfer_start = esp_timer_get_time();
        static uint32_t timing_ms = 0;
        const bool trace = request.display == nullptr &&
            static_cast<uint32_t>(transfer_start / 1000) - timing_ms >= 5000;
        uint64_t pack_us = 0;
        WatcherLcdIoSetPacking(panel_io_, lease != 0 ? PackNativeFrame : nullptr,
                              trace ? &pack_us : nullptr, scanout_,
                              scanout_ ? kScanoutBlockBytes : 0);
        // LVGL areas include x2; esp_lcd's x_end is exclusive. Never send an
        // unaligned window to SPD2010, even if a future caller skips rounder_cb.
        const bool aligned = request.area.x1 % 4 == 0 &&
            (request.area.x2 + 1) % 4 == 0;
        const esp_err_t err = aligned ? esp_lcd_panel_draw_bitmap(
            panel_, request.area.x1, request.area.y1, request.area.x2 + 1,
            request.area.y2 + 1, request.pixels) : ESP_ERR_INVALID_ARG;
        const int64_t transfer_submitted = esp_timer_get_time();
        WatcherLcdIoSetPacking(panel_io_, nullptr, nullptr, nullptr, 0);
        if (err != ESP_OK) {
            const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
            if (static_cast<uint32_t>(now_ms - flush_error_ms_) > 1000) {
                flush_error_ms_ = now_ms;
                ESP_LOGE(kTag, "flush failed (%s): %dx%d at %d,%d",
                         esp_err_to_name(err),
                         (int)(request.area.x2 - request.area.x1 + 1),
                         (int)(request.area.y2 - request.area.y1 + 1),
                         (int)request.area.x1, (int)request.area.y1);
            }
        } else {
            // Keep the draw buffer immutable until the actual transfer ends.
            // The ISR only publishes a timestamp and wakes this existing task;
            // Host counters are deliberately updated outside interrupt context.
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const auto completed_us = pxa_board_common::DisplayCompletionTimestamp(
                static_cast<uint64_t>(esp_timer_get_time()),
                flush_done_us_low_.load(std::memory_order_acquire));
            const uint32_t now_ms = completed_us / 1000;
            if (trace) {
                timing_ms = now_ms;
                ESP_LOGI(kTag, "Direct transfer: submit=%lld us complete=%llu us pack=%llu us",
                    static_cast<long long>(transfer_submitted - transfer_start),
                    static_cast<unsigned long long>(completed_us - transfer_start),
                    static_cast<unsigned long long>(pack_us));
            }
            if (request.last) {
                pxa_board_performance_note_frame();
                pxa_esp_surface_note_display_latency(
                    request.frame_id, request.input_timestamp_us,
                    request.raster_ready_tick_us, completed_us);
                pxa_esp_surface_note_game_frame_presented(request.frame_id, completed_us);
                uint16_t expected = request.snapshot_revision;
                if (expected != 0 && snapshot_pending_.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel)) {
                    snapshot_timestamp_us_.store(completed_us, std::memory_order_relaxed);
                    snapshot_ready_.store(true, std::memory_order_release);
                    if (snapshot_sem_ != nullptr) xSemaphoreGive(snapshot_sem_);
                }
            }
        }
        if (request.display != nullptr)
            ui_flush_done_.store(true, std::memory_order_release);
        if (request.display == nullptr) {
            if (lease != 0) {
                pxa_esp_surface_release_frame(lease);
                direct_lease_ = 0;
            }
            direct_snapshot_current_ = err == ESP_OK;
            direct_native_current_ = lease != 0;
            snapshot_timestamp_us_.store(pxa_board_common::DisplayCompletionTimestamp(
                static_cast<uint64_t>(esp_timer_get_time()),
                flush_done_us_low_.load(std::memory_order_acquire)), std::memory_order_relaxed);
            // Frame-ready notifications can arrive while DMA runs. Use the
            // existing capture semaphore for completion, not that notification.
            xSemaphoreGive(snapshot_sem_);
        }
    }
}

bool SensecapWatcherHardware::PresentDirectFrame() {
    // Called under LVGL's lock. Native frames use the bounded SRAM staging
    // blocks; composition reuses the existing mirror. No new full frame/task.
    if (snapshot_ == nullptr || snapshot_sem_ == nullptr ||
        snapshot_pending_.load(std::memory_order_acquire) != 0 ||
        !screen_enabled_.load() || reference_ui_ == nullptr ||
        pxsys_reference_lvgl_is_locked(reference_ui_)) return false;
    WaitDirectFrame();
    pxa_esp_surface_frame_t frame;
    if (!pxa_esp_surface_acquire_prepared_with_alpha(&frame)) return false;
    if (!watcher_pxa_surface::DirectFrameEligible(frame)) {
        pxa_esp_surface_release_frame(frame.lease);
        return false;
    }
    const lv_area_t area = {0, 0, WATCHER_DISPLAY_WIDTH - 1, WATCHER_DISPLAY_HEIGHT - 1};
    const auto visible = [](const pxa_esp_surface_ui_alpha_plane_t& plane) {
        return plane.visible && plane.opacity != 0;
    };
    const bool native = scanout_ != nullptr && frame.width == WATCHER_DISPLAY_WIDTH &&
        frame.height == WATCHER_DISPLAY_HEIGHT &&
        frame.stride_bytes == WATCHER_DISPLAY_WIDTH * 2 &&
        (frame.flags & PXA_SURFACE_FLAG_GUEST_MAPPED) == 0 &&
        (!visible(frame.ui_alpha_plane) || frame.suppress_guest_alpha) &&
        !visible(frame.system_alpha_plane);
    if (native) {
        // Pack straight from the leased Surface into two small DMA blocks.
        // No whole-frame mirror copy; the next raster may use another buffer.
        direct_lease_ = frame.lease;
        (void)xSemaphoreTake(snapshot_sem_, 0);
        const FlushRequest request{nullptr, area, const_cast<uint8_t*>(frame.pixels),
            frame.frame_id, frame.input_timestamp_us, frame.raster_ready_tick_us, true, 0};
        if (xQueueSend(flush_queue_, &request, portMAX_DELAY) != pdTRUE) {
            direct_lease_ = 0;
            pxa_esp_surface_release_frame(frame.lease);
            return false;
        }
        direct_frame_pending_ = true;
        return true;
    }
    const int64_t compose_start = esp_timer_get_time();
    const auto composed = watcher_pxa_surface::ComposeFrame(
        frame, &area, reinterpret_cast<uint8_t*>(snapshot_));
    static uint32_t timing_ms = 0;
    const int64_t composed_us = esp_timer_get_time();
    const uint32_t now_ms = composed_us / 1000;
    if (now_ms - timing_ms >= 5000) {
        timing_ms = now_ms;
        ESP_LOGI(kTag, "Direct compose: copy=%lld us alpha=%u,%u",
            static_cast<long long>(composed_us - compose_start),
            frame.ui_alpha_plane.visible, frame.system_alpha_plane.visible);
    }
    pxa_esp_surface_release_frame(frame.lease);
    pxa_board_performance_draw_rgb565(snapshot_, WATCHER_DISPLAY_WIDTH,
        WATCHER_DISPLAY_HEIGHT, WATCHER_DISPLAY_WIDTH, 0, 0,
        WATCHER_DISPLAY_WIDTH, WATCHER_DISPLAY_HEIGHT, true);
    (void)xSemaphoreTake(snapshot_sem_, 0);
    const FlushRequest request = {nullptr, area, reinterpret_cast<uint8_t*>(snapshot_),
        composed.frame_id, composed.input_timestamp_us, composed.raster_ready_tick_us,
        true, 0};
    // A previous LVGL transfer may still be in flight. The existing queue
    // serializes panel access. Return after queuing so the next raster overlaps
    // DMA; every mirror reader/writer drains pending before accessing it.
    if (xQueueSend(flush_queue_, &request, portMAX_DELAY) != pdTRUE) return false;
    direct_frame_pending_ = true;
    return true;
}

void SensecapWatcherHardware::WaitDirectFrame() {
    if (!direct_frame_pending_) return;
    (void)xSemaphoreTake(snapshot_sem_, portMAX_DELAY);
    direct_frame_pending_ = false;
}

bool SensecapWatcherHardware::CopyDirectSnapshot(uint16_t* pixels) {
    const size_t count = WATCHER_DISPLAY_WIDTH * WATCHER_DISPLAY_HEIGHT;
    if (direct_native_current_) {
        pxa_esp_surface_frame_t frame{};
        if (!pxa_esp_surface_acquire_current_for_preview(&frame)) return false;
        if (frame.format != PXA_SURFACE_FORMAT_RGB565 ||
            frame.width != WATCHER_DISPLAY_WIDTH || frame.height != WATCHER_DISPLAY_HEIGHT ||
            frame.stride_bytes != WATCHER_DISPLAY_WIDTH * sizeof(uint16_t)) {
            pxa_esp_surface_release_frame(frame.lease);
            return false;
        }
        memcpy(pixels, frame.pixels, count * sizeof(uint16_t));
        pxa_esp_surface_release_frame(frame.lease);
        pxa_board_performance_draw_rgb565(pixels, WATCHER_DISPLAY_WIDTH,
            WATCHER_DISPLAY_HEIGHT, WATCHER_DISPLAY_WIDTH, 0, 0,
            WATCHER_DISPLAY_WIDTH, WATCHER_DISPLAY_HEIGHT, false);
    } else {
        for (size_t index = 0; index < count; ++index)
            pixels[index] = __builtin_bswap16(snapshot_[index]);
    }
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
    if (watcher_spd2010_touch_new(touch_io, &touch_config, &touch) !=
        ESP_OK) {
        ESP_LOGE(kTag, "Cannot create SPD2010 touch driver");
        return false;
    }
    // Prime the controller the way the official BSP does before adding the
    // input device.
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(touch);
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
    boot_screen_.Hide();
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
                            (void)pxa_host_prepare_shutdown(1200);
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
                pxa_esp_surface_set_power_overlay_visible(visible);
            });
        OnLockChanged(pxsys_reference_lvgl_is_locked(reference_ui_));
    }
    PublishStatus();
}

void SensecapWatcherHardware::SetBrightness(uint8_t percent) {
    backlight_.SetPercent(percent);
    ApplyBacklight();
    ScheduleStatusUpdate();
}

void SensecapWatcherHardware::ApplyBacklight() {
    backlight_.Apply(screen_enabled_.load());
}

void SensecapWatcherHardware::SetIdleDim(bool enabled, uint8_t percent) {
    backlight_.SetIdleDim(enabled, percent);
    ApplyBacklight();
}

void SensecapWatcherHardware::AutoScreenOff() {
    if (!screen_enabled_.exchange(false)) return;
    pxa_esp_surface_set_display_unlocked(false);
    (void)pxa_host_set_display_interactive(false);
    ApplyBacklight();
}

void SensecapWatcherHardware::SetVolume(uint8_t percent) {
    audio_.SetVolume(percent);
    ScheduleStatusUpdate();
}

void SensecapWatcherHardware::PublishStatus() {
    if (system_ == nullptr) return;
    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    (void)pxa_system_clock_update_status(&status);
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
    status.brightness_percent = backlight_.percent();
    (void)pxsys_system_status_service_update(
        pxsys_standard_system_status(system_), &status);
}

bool SensecapWatcherHardware::InitializeWifi() {
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected && time_sync_initialized_.load()) {
            const auto result = esp_netif_sntp_start();
            if (result != ESP_OK)
                ESP_LOGW(kTag, "Unable to start time sync: %s", esp_err_to_name(result));
        }
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
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    clock.start = false;
    clock.wait_for_sync = false;
    clock.sync_cb = [](struct timeval*) { ESP_LOGI(kTag, "System time synchronized"); };
    const auto result = esp_netif_sntp_init(&clock);
    time_sync_initialized_.store(result == ESP_OK);
    if (result != ESP_OK)
        ESP_LOGW(kTag, "Unable to initialize time sync: %s", esp_err_to_name(result));
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
    SetBrightness(brightness());
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
#if CONFIG_PXADB_TEST_CONTROL
    for (const char* name : {"taskLVGL", "watcher_flush", "watcher_pxa"}) {
        if (const auto task = xTaskGetHandle(name)) {
            ESP_LOGI(kTag, "Stack[%s]: minimum_free=%u B", name,
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
        }
    }
#endif
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
    pxa_esp_surface_set_display_unlocked(false);
    if (!enabled) (void)pxa_host_set_display_interactive(false);
    if (!enabled) {
        ApplyBacklight();
    }
    lv_lock();
    const lv_result_t result = lv_async_call(
        [](void* context) {
            auto* self = static_cast<SensecapWatcherHardware*>(context);
            if (self->reference_ui_ != nullptr)
                (void)pxsys_reference_lvgl_set_locked(self->reference_ui_, true);
            if (!self->screen_enabled_.load()) return;
            lv_display_trigger_activity(self->display_);
            self->SetIdleDim(false, 100);
            if (self->display_ != nullptr) lv_refr_now(self->display_);
            self->SetBrightness(self->backlight_.percent() == 0
                                    ? kDefaultBrightness
                                    : self->backlight_.percent());
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
    const bool interactive = !locked && screen_enabled_.load();
    pxa_esp_surface_set_display_unlocked(interactive);
    (void)pxa_host_set_display_interactive(interactive);
}

void SensecapWatcherHardware::SnapshotFlushArea(const lv_area_t* area,
                                                const uint8_t* pixels) {
    if (snapshot_ == nullptr || area == nullptr || pixels == nullptr) return;
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    if (width <= 0 || height <= 0) return;
    // The panel runs LV_COLOR_FORMAT_RGB565_SWAPPED, so the draw buffer holds
    // byte-swapped pixels; the capture reports plain little-endian RGB565.
    // LVGL partial mode packs this area, even with a full-screen allocation.
    // Using the panel width as the source stride corrupts narrow captures.
    const auto* source = reinterpret_cast<const uint16_t*>(pixels);
    for (int row = 0; row < height; ++row) {
        const int y = area->y1 + row;
        if (y < 0 || y >= WATCHER_DISPLAY_HEIGHT) continue;
        const uint16_t* src =
            source + static_cast<size_t>(row) * width;
        uint16_t* dst =
            snapshot_ + static_cast<size_t>(y) * WATCHER_DISPLAY_WIDTH;
        for (int column = 0; column < width; ++column) {
            const int x = area->x1 + column;
            if (x < 0 || x >= WATCHER_DISPLAY_WIDTH) continue;
            const uint16_t value = src[column];
            dst[x] = static_cast<uint16_t>((value >> 8) | (value << 8));
        }
    }
}

bool SensecapWatcherHardware::CaptureRgb565(
    uint16_t* pixels, size_t pixel_count, bool after_present,
    pxadb::TestControlCaptureInfo* info) {
    if (pixels == nullptr || snapshot_ == nullptr) return false;
    const size_t needed =
        static_cast<size_t>(WATCHER_DISPLAY_WIDTH) * WATCHER_DISPLAY_HEIGHT;
    if (pixel_count < needed) return false;
    // Direct games capture their completed Surface or composed mirror. The
    // fallback UI populates the mirror through a fresh completed redraw.
    (void)after_present;
    {
        if (snapshot_sem_ == nullptr || !lvgl_port_lock(1000)) return false;
        lv_lock();
        WaitDirectFrame();
        if (direct_snapshot_current_ && pxa_esp_surface_has_visible_surface() &&
            !pxa_esp_surface_composition_required()) {
            // Export the completed native Surface or composed mirror without
            // forcing a redraw. Both owners are blocked during this copy.
            if (!CopyDirectSnapshot(pixels)) {
                lv_unlock();
                lvgl_port_unlock();
                return false;
            }
            if (info != nullptr) {
                info->width = WATCHER_DISPLAY_WIDTH;
                info->height = WATCHER_DISPLAY_HEIGHT;
                info->stride_bytes = WATCHER_DISPLAY_WIDTH * sizeof(uint16_t);
                info->frame_id = snapshot_frame_id_.fetch_add(1, std::memory_order_relaxed) + 1;
                info->completed_timestamp_us = snapshot_timestamp_us_.load(std::memory_order_relaxed);
                info->source = "pxa-direct";
            }
            lv_unlock();
            lvgl_port_unlock();
            return true;
        }
        (void)xSemaphoreTake(snapshot_sem_, 0);
        uint32_t revision = snapshot_frame_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (static_cast<uint16_t>(revision) == 0)
            revision = snapshot_frame_id_.fetch_add(1, std::memory_order_relaxed) + 1;
        snapshot_ready_.store(false, std::memory_order_release);
        snapshot_pending_.store(static_cast<uint16_t>(revision), std::memory_order_release);
        // A fresh full redraw supplies every pixel without copying every
        // ordinary flush. Install the token under LVGL's lock so no partial
        // refresh can be mistaken for this requested frame.
        lv_obj_invalidate(lv_display_get_screen_active(display_));
        lv_unlock();
        lvgl_port_unlock();
        if (xSemaphoreTake(snapshot_sem_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            uint16_t expected = static_cast<uint16_t>(revision);
            (void)snapshot_pending_.compare_exchange_strong(expected, 0,
                                                           std::memory_order_acq_rel);
            return false;
        }
    }
    // The presenter also uses this mirror. Keep it immutable during export,
    // including if a direct frame replaced the requested LVGL snapshot.
    if (!lvgl_port_lock(1000)) return false;
    WaitDirectFrame();
    if (direct_snapshot_current_) {
        lv_lock();
        const bool copied = CopyDirectSnapshot(pixels);
        lv_unlock();
        if (!copied) { lvgl_port_unlock(); return false; }
    } else {
        memcpy(pixels, snapshot_, needed * sizeof(uint16_t));
    }
    if (info != nullptr) {
        info->width = WATCHER_DISPLAY_WIDTH;
        info->height = WATCHER_DISPLAY_HEIGHT;
        info->stride_bytes = WATCHER_DISPLAY_WIDTH * sizeof(uint16_t);
        info->frame_id = snapshot_frame_id_.load(std::memory_order_relaxed);
        info->completed_timestamp_us =
            snapshot_timestamp_us_.load(std::memory_order_relaxed);
        info->source = "lvgl-flush";
    }
    lvgl_port_unlock();
    return true;
}

void SensecapWatcherHardware::NavigateHome() {
    if (!screen_enabled_.load()) {
        WakeScreen();
        return;
    }
    if (reference_ui_ != nullptr &&
        pxsys_reference_lvgl_is_locked(reference_ui_)) {
        return;
    }
    lv_lock();
    lv_async_call([](void* context) {
        auto* self = static_cast<SensecapWatcherHardware*>(context);
        if (self->reference_ui_ != nullptr) {
            (void)pxsys_reference_lvgl_home(self->reference_ui_);
        }
    }, this);
    lv_unlock();
}

bool SensecapWatcherHardware::RouteInjectedPointerDown() {
    if (screen_enabled_.load()) return true;
    WakeScreen();
    return false;
}

bool SensecapWatcherHardware::InjectPointer(uint8_t id, uint16_t x, uint16_t y,
                                            bool pressed) {
    if (display_ == nullptr || id >= kDebugPointers || x >= WATCHER_DISPLAY_WIDTH ||
        y >= WATCHER_DISPLAY_HEIGHT) return false;
    if (injected_pointers_[id] == nullptr) {
        if (!lvgl_port_lock(1000)) return false;
        lv_lock();
        auto* indev = lv_indev_create();
        if (indev != nullptr) {
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(indev, display_);
            lv_indev_set_driver_data(indev, this);
            lv_indev_set_user_data(indev, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
            lv_indev_set_read_cb(indev, ReadInjectedPointer);
            lv_timer_set_period(lv_indev_get_read_timer(indev), kTouchPollMs);
            injected_pointers_[id] = indev;
        }
        lv_unlock();lvgl_port_unlock();
        if (indev == nullptr) return false;
    }
    portENTER_CRITICAL(&injected_lock_);
    const bool accepted = injected_touches_.Push(id, x, y, pressed);
    portEXIT_CRITICAL(&injected_lock_);
    return accepted;
}

bool SensecapWatcherHardware::CancelInjectedPointer(uint8_t id) {
    portENTER_CRITICAL(&injected_lock_);
    const bool accepted = injected_touches_.Cancel(id);
    portEXIT_CRITICAL(&injected_lock_);
    return accepted;
}

void SensecapWatcherHardware::ReadInjectedPointer(lv_indev_t* indev,
                                                  lv_indev_data_t* data) {
    auto* self = static_cast<SensecapWatcherHardware*>(lv_indev_get_driver_data(indev));
    if (self == nullptr || data == nullptr) return;
    const auto id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_indev_get_user_data(indev)));
    bool more = false;
    portENTER_CRITICAL(&self->injected_lock_);
    const auto sample = self->injected_touches_.Read(id, more);
    portEXIT_CRITICAL(&self->injected_lock_);
    if (sample.cancel) lv_indev_wait_release(indev);
    data->timestamp = lv_tick_get();
    data->point = {sample.x, sample.y};
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = more;
}

bool SensecapWatcherHardware::RouteInjectedKey(pxadb::TestControlKey key) {
    switch (key) {
        case pxadb::TestControlKey::kBack:
        case pxadb::TestControlKey::kHome:
            NavigateHome();
            return true;
        case pxadb::TestControlKey::kVolumeUp:
            AdjustVolume(kVolumeStep);
            return true;
        case pxadb::TestControlKey::kVolumeDown:
            AdjustVolume(-kVolumeStep);
            return true;
    }
    return false;
}

bool SensecapWatcherHardware::ConfigurePxadbControls() {
    pxadb::PowerControlAdapter power_adapter;
    power_adapter.context = this;
    power_adapter.power_off = [](void* context) {
        return static_cast<SensecapWatcherHardware*>(context)->RequestPowerOff();
    };
    if (pxadb::ConfigurePowerControl(&power_adapter) != ESP_OK) return false;

    pxadb::TestControlAdapter adapter;
    adapter.context = this;
    adapter.width = WATCHER_DISPLAY_WIDTH;
    adapter.height = WATCHER_DISPLAY_HEIGHT;
    adapter.max_pointers = kDebugPointers;
    adapter.route_pointer_down = [](void* context) {
        return static_cast<SensecapWatcherHardware*>(context)
            ->RouteInjectedPointerDown();
    };
    adapter.inject_pointer = [](void* context, uint8_t id, uint16_t x, uint16_t y,
                                bool pressed) {
        return static_cast<SensecapWatcherHardware*>(context)
            ->InjectPointer(id, x, y, pressed);
    };
    adapter.cancel_pointer = [](void* context, uint8_t id) {
        return static_cast<SensecapWatcherHardware*>(context)
            ->CancelInjectedPointer(id);
    };
    adapter.route_key = [](void* context, pxadb::TestControlKey key) {
        return static_cast<SensecapWatcherHardware*>(context)
            ->RouteInjectedKey(key);
    };
    adapter.capture_rgb565 = [](
        void* context, uint16_t* pixels, size_t pixel_count,
        bool after_present, pxadb::TestControlCaptureInfo* info) {
        return static_cast<SensecapWatcherHardware*>(context)->CaptureRgb565(
            pixels, pixel_count, after_present, info);
    };
    const esp_err_t result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}

bool SensecapWatcherHardware::RequestPowerOff() {
    PowerOff();
    return true;
}

void SensecapWatcherHardware::PowerOff() {
    (void)pxa_host_prepare_shutdown(1200);
    audio_.StopCaptureForShutdown();
    (void)pxa_system_clock_save();
    ESP_LOGI(kTag, "Releasing the power latch");
    ledc_set_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, WATCHER_LCD_BACKLIGHT_CHANNEL);
    io_expander_.SetOutputs(WATCHER_IO_PWR_SYSTEM, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // With USB attached the rails stay up; restart instead of hanging dark.
    esp_restart();
}
