#include "esp_mosaico_hardware.h"
#include "esp_mosaico_transfer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_lcd_co5300.h>
#include <esp_lcd_touch_cst92xx.h>
#include <esp_lv_decoder.h>
#include <esp_lv_adapter.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <button_gpio.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_console.h>
#include <tinyusb_default_config.h>
#include <wifi_manager.h>

#include "esp_mosaico_pxa_surface.h"

namespace {

constexpr char kTag[] = "mosaico_hw";
constexpr size_t kFramePixels = mosaico_board::kWidth * mosaico_board::kHeight;

esp_err_t ConfigureOutput(int pin, int level,
                           gpio_mode_t mode = GPIO_MODE_OUTPUT) {
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(static_cast<gpio_num_t>(pin), level),
                        kTag, "set output level");
    return gpio_config(&config);
}

uint8_t SignalLevel(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

}

bool EspMosaicoHardware::Initialize() {
    if (!InitializeUsb()) ESP_LOGW(kTag, "USB CDC console unavailable");
    if (!DetectHardware() || !InitializePower() || !InitializeI2c() ||
        !InitializeDisplay())
        return false;
    ShowBootFrame();
    if (!InitializeTouch()) ESP_LOGW(kTag, "Continuing without CST92xx touch");
    if (!InitializeButton()) ESP_LOGW(kTag, "Function button unavailable");
    audio_initialized_ = audio_.Initialize(i2c_bus_);
    if (!audio_initialized_) ESP_LOGW(kTag, "Continuing without ES8311 audio");
    wifi_initialized_ = InitializeWifi();
    if (!wifi_initialized_) ESP_LOGW(kTag, "Continuing without Wi-Fi");
    return true;
}

bool EspMosaicoHardware::DetectHardware() {
    uint16_t version = 0;
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version,
                                 sizeof(version) * 8) != ESP_OK)
        return false;
    hardware_ = mosaico_board::DecodeHardwareConfig(version);
    if (!hardware_) {
        ESP_LOGE(kTag, "Unsupported hardware version 0x%04x", version);
        return false;
    }
    ESP_LOGI(kTag, "%s: LCD reset=%d clock=%d, I2C SDA=%d SCL=%d",
             hardware_->name, hardware_->display_reset, hardware_->display_clock,
             hardware_->i2c_data, hardware_->i2c_clock);
    return true;
}

bool EspMosaicoHardware::InitializeUsb() {
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.task.xCoreID = 0;
    if (tinyusb_driver_install(&config) != ESP_OK) return false;
    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr,
    };
    if (tinyusb_cdcacm_init(&cdc_config) != ESP_OK) return false;
    if (tinyusb_console_init(TINYUSB_CDC_ACM_0) != ESP_OK) return false;
    const int output_status = std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int error_status = std::setvbuf(stderr, nullptr, _IONBF, 0);
    return output_status == 0 && error_status == 0;
}

bool EspMosaicoHardware::InitializePower() {
    if (ConfigureOutput(mosaico_board::kPeripheralPower, 1) != ESP_OK ||
        ConfigureOutput(mosaico_board::kPowerSwitch, 1,
                         GPIO_MODE_OUTPUT_OD) != ESP_OK)
        return false;
    if (hardware_->codec_power >= 0 &&
        ConfigureOutput(hardware_->codec_power, 0) != ESP_OK)
        return false;
    if (gpio_hold_dis(static_cast<gpio_num_t>(mosaico_board::kPeripheralPower)) != ESP_OK ||
        gpio_hold_dis(static_cast<gpio_num_t>(mosaico_board::kPowerSwitch)) != ESP_OK)
        return false;
    if (hardware_->codec_power >= 0 &&
        gpio_hold_dis(static_cast<gpio_num_t>(hardware_->codec_power)) != ESP_OK)
        return false;

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 100000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = mosaico_board::kPeripheralPower,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 255,
        .hpoint = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK ||
        ledc_channel_config(&channel) != ESP_OK)
        return false;
    const esp_err_t installed = ledc_fade_func_install(0);
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
        vSemaphoreDelete(te_ready_); te_ready_ = nullptr; return false;
    }
    esp_err_t result = ledc_set_fade_with_time(
        LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0, 50);
    if (result == ESP_OK)
        result = ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1,
                                 LEDC_FADE_WAIT_DONE);
    if (installed == ESP_OK) ledc_fade_func_uninstall();
    (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    if (result != ESP_OK ||
        ConfigureOutput(mosaico_board::kPeripheralPower, 0) != ESP_OK)
        return false;
    if (hardware_->codec_power >= 0 &&
        gpio_set_level(static_cast<gpio_num_t>(hardware_->codec_power), 1) != ESP_OK)
        return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    return true;
}

bool EspMosaicoHardware::InitializeI2c() {
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = static_cast<gpio_num_t>(hardware_->i2c_data),
        .scl_io_num = static_cast<gpio_num_t>(hardware_->i2c_clock),
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&config, &i2c_bus_) == ESP_OK;
}

bool EspMosaicoHardware::InitializeDisplay() {
    static const uint8_t kPage20[] = {0x20};
    static const uint8_t kRegister19[] = {0x10};
    static const uint8_t kRegister1c[] = {0xa0};
    static const uint8_t kPage00[] = {0x00};
    static const uint8_t kRegisterC4[] = {0x80};
    static const uint8_t kPixelFormat[] = {0x55};
    static const uint8_t kTearEffect[] = {0x00};
    static const uint8_t kDisplayControl[] = {0x20};
    static const uint8_t kMaximumBrightness[] = {0xff};
    static const uint8_t kRange[] = {0x00, 0x00, 0x01, 0xdf};
    static const co5300_lcd_init_cmd_t kInitCommands[] = {
        {0x11, nullptr, 0, 120},
        {0xfe, kPage20, 1, 0},
        {0x19, kRegister19, 1, 0},
        {0x1c, kRegister1c, 1, 0},
        {0xfe, kPage00, 1, 0},
        {0xc4, kRegisterC4, 1, 0},
        {0x3a, kPixelFormat, 1, 0},
        {0x35, kTearEffect, 1, 0},
        {0x53, kDisplayControl, 1, 0},
        {0x51, kMaximumBrightness, 1, 0},
        {0x63, kMaximumBrightness, 1, 0},
        {0x2a, kRange, 4, 0},
        {0x2b, kRange, 4, 0},
    };
    spi_bus_config_t bus_config = {};
    bus_config.data0_io_num = mosaico_board::kDisplayData0;
    bus_config.data1_io_num = mosaico_board::kDisplayData1;
    bus_config.data2_io_num = mosaico_board::kDisplayData2;
    bus_config.data3_io_num = mosaico_board::kDisplayData3;
    bus_config.sclk_io_num = hardware_->display_clock;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    bus_config.max_transfer_sz = mosaico_board::kWidth *
        mosaico_board::kTransferRows * sizeof(uint16_t);
    if (spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO) != ESP_OK)
        return false;
    const int pins[] = {hardware_->display_clock, mosaico_board::kDisplayChipSelect,
                        mosaico_board::kDisplayData0, mosaico_board::kDisplayData1,
                        mosaico_board::kDisplayData2, mosaico_board::kDisplayData3};
    for (int pin : pins) {
        if (gpio_set_drive_capability(static_cast<gpio_num_t>(pin),
                                       GPIO_DRIVE_CAP_3) != ESP_OK)
            return false;
    }
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = static_cast<gpio_num_t>(mosaico_board::kDisplayChipSelect),
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = mosaico_board::kPixelClockHz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {.quad_mode = true},
    };
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io_) != ESP_OK)
        return false;
    co5300_vendor_config_t vendor = {};
    vendor.init_cmds = kInitCommands;
    vendor.init_cmds_size = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
    vendor.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = static_cast<gpio_num_t>(hardware_->display_reset);
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor;
    if (esp_lcd_new_panel_co5300(panel_io_, &panel_config, &panel_) != ESP_OK ||
        esp_lcd_panel_reset(panel_) != ESP_OK ||
        esp_lcd_panel_init(panel_) != ESP_OK)
        return false;
    transfer_done_ = xSemaphoreCreateBinary();
    present_available_ = xSemaphoreCreateBinary();
    panel_mutex_ = xSemaphoreCreateMutex();
    flush_queue_ = xQueueCreate(1, sizeof(FlushRequest));
    for (auto& buffer : transfer_pixels_) {
        buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            64, mosaico_board::kWidth * mosaico_board::kTransferRows * sizeof(uint16_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }
    present_pixels_ = static_cast<uint8_t*>(heap_caps_aligned_calloc(
        64, kFramePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    scanout_pixels_ = static_cast<uint8_t*>(heap_caps_aligned_calloc(
        64, kFramePixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (transfer_done_ == nullptr || present_available_ == nullptr ||
        panel_mutex_ == nullptr || flush_queue_ == nullptr ||
        transfer_pixels_[0] == nullptr || transfer_pixels_[1] == nullptr ||
        present_pixels_ == nullptr ||
        scanout_pixels_ == nullptr)
        return false;
    xSemaphoreGive(present_available_);

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = 5;
    adapter_config.task_stack_size = 12288;
    adapter_config.task_core_id = 1;
    adapter_config.task_max_delay_ms = 16;
    adapter_config.tick_period_ms = 2;
    if (esp_lv_adapter_init(&adapter_config) != ESP_OK ||
        esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false) != ESP_OK)
        return false;
    esp_lv_adapter_display_config_t display_config = {};
    display_config.panel_io = panel_io_;
    display_config.panel = panel_;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.hor_res = mosaico_board::kWidth;
    display_config.profile.ver_res = mosaico_board::kHeight;
    display_config.profile.buffer_height = mosaico_board::kHeight;
    display_config.profile.use_psram = true;
    display_config.profile.require_double_buffer = true;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.te_sync.gpio_num = -1;
    if (esp_lv_adapter_lock(1000) != ESP_OK) return false;
    display_ = esp_lv_adapter_register_display(&display_config);
    if (display_ == nullptr) {
        esp_lv_adapter_unlock();
        return false;
    }
    lv_display_set_driver_data(display_, this);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(display_, FlushDisplay);
    // Return draw buffers after copying; the panel reads an owned PSRAM
    // snapshot while LVGL renders the next refresh. Limit only this display.
    lv_display_set_flush_wait_cb(display_, nullptr);
    lv_timer_set_period(lv_display_get_refr_timer(display_), 33);
    lv_display_add_event_cb(display_, [](lv_event_t* event) {
        auto* self = static_cast<EspMosaicoHardware*>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
            self->refresh_started_us_ = esp_timer_get_time();
            self->refresh_wait_us_ = 0;
            self->flush_direct_checked_ = false;
            self->flush_direct_ready_ = false;
        } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY) {
            const uint32_t duration = esp_timer_get_time() - self->refresh_started_us_ -
                                      self->refresh_wait_us_;
            self->render_duration_us_ = duration;
            self->render_max_us_ = std::max(self->render_max_us_.load(), duration);
        }
    }, LV_EVENT_ALL, this);
    lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<EspMosaicoHardware*>(lv_timer_get_user_data(timer));
        if (self->redraw_pending_.exchange(false))
            lv_obj_invalidate(lv_display_get_screen_active(self->display_));
    }, 16, this);
    const esp_err_t rounded = esp_lv_adapter_set_area_rounder_cb(
        display_, [](lv_area_t* area, void*) { RoundDisplayArea(area); }, this);
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = OnColorTransferDone,
    };
    const esp_err_t registered = esp_lcd_panel_io_register_event_callbacks(
        panel_io_, &callbacks, this);
    esp_lv_decoder_handle_t decoder = nullptr;
    const esp_err_t decoder_result = esp_lv_decoder_init(&decoder);
    esp_lv_adapter_unlock();
    if (registered != ESP_OK || decoder_result != ESP_OK || rounded != ESP_OK)
        return false;
    // Short copy/command bursts must not wait behind the LVGL draw workers.
    // The task sleeps during TE and DMA waits, leaving CPU time for rendering.
    if (xTaskCreatePinnedToCore(FlushTask, "mosaico_flush", 4096, this, 6,
                    &flush_task_, 0) != pdPASS) return false;
    if (esp_lv_adapter_start() != ESP_OK ||
        !mosaico_pxa_surface::Install(display_, [](void* context) {
            return static_cast<EspMosaicoHardware*>(context)->PresentDirectFrame();
        }, this)) return false;
    if (!InitializeTe()) ESP_LOGW(kTag, "TE unavailable; using unsynchronized refresh");
    return ApplyBrightness();
}

bool EspMosaicoHardware::InitializeTe() {
    te_ready_ = xSemaphoreCreateBinary();
    if (!te_ready_) return false;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << mosaico_board::kDisplayTe;
    config.mode = GPIO_MODE_INPUT;
    config.intr_type = GPIO_INTR_ANYEDGE;
    if (gpio_config(&config) != ESP_OK) {
        vSemaphoreDelete(te_ready_); te_ready_ = nullptr; return false;
    }
    // Share the ISR service with the touch/button drivers. No IRAM flag is
    // requested because the semaphore and this context use normal SRAM.
    const esp_err_t installed = gpio_install_isr_service(0);
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) return false;
    if (gpio_isr_handler_add(static_cast<gpio_num_t>(mosaico_board::kDisplayTe),
                             OnTe, this) != ESP_OK) {
        vSemaphoreDelete(te_ready_); te_ready_ = nullptr; return false;
    }
    ESP_LOGI(kTag, "TE scan-follow synchronization on GPIO%d (falling edge)",
             mosaico_board::kDisplayTe);
    return true;
}

void EspMosaicoHardware::OnTe(void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    if (gpio_get_level(static_cast<gpio_num_t>(mosaico_board::kDisplayTe))) {
        self->te_rise_us_ = now;
        return;
    }
    const uint32_t last = self->te_timestamp_us_.load();
    if (last != 0 && now - last < 5000) return;
    const uint32_t rise = self->te_rise_us_.exchange(0);
    const uint32_t period = self->te_period_us_.load();
    if (rise != 0 && now - rise < (period != 0 ? period / 2 : 5000))
        self->te_blank_us_ = now - rise;
    const uint32_t previous = self->te_timestamp_us_.exchange(now);
    if (self->te_edges_.fetch_add(1) != 0 && now - previous < 100000)
        self->te_period_us_ = now - previous;
    BaseType_t awakened = pdFALSE;
    xSemaphoreGiveFromISR(self->te_ready_, &awakened);
    if (awakened) portYIELD_FROM_ISR();
}

void EspMosaicoHardware::WaitForTe(const lv_area_t& area) {
    scan_deadline_us_ = 0;
    // Panel-off initialization has no TE pulses. Retry a missing signal once
    // a second, rather than delaying every frame by the timeout indefinitely.
    const int64_t now = esp_timer_get_time();
    if (!te_ready_ || !panel_on_.load() || now < te_retry_us_) return;
    (void)xSemaphoreTake(te_ready_, 0);
    const int64_t deadline = now + 50000;
    while (esp_timer_get_time() < deadline) {
        const int64_t remaining = deadline - esp_timer_get_time();
        const TickType_t ticks = std::max<TickType_t>(1,
            pdMS_TO_TICKS(std::max<int64_t>(1, (remaining + 999) / 1000)));
        if (xSemaphoreTake(te_ready_, ticks) != pdTRUE) break;
        // Reject pulses whose safe start was lost to a scheduling delay.
        const uint32_t age = static_cast<uint32_t>(esp_timer_get_time()) -
                             te_timestamp_us_.load();
        if (age <= 2000) {
            const uint32_t measured = te_period_us_.load();
            const uint32_t period = measured != 0 ? measured : 16667;
            const uint32_t blank = te_blank_us_.load();
            const uint32_t active = period > blank ? period - blank : period;
            const int64_t edge = esp_timer_get_time() - age;
            const int64_t start = edge +
                mosaico_board::ScanFollowDelayUs(area.y1, active);
            scan_deadline_us_ = edge + period +
                static_cast<uint64_t>(area.y2) * active / mosaico_board::kHeight - 250;
            for (;;) {
                const int64_t remaining_us = start - esp_timer_get_time();
                if (remaining_us <= 0) break;
                vTaskDelay(std::max<TickType_t>(1,
                    pdMS_TO_TICKS((remaining_us + 999) / 1000)));
            }
            return;
        }
    }
    const uint32_t timeouts = ++te_timeouts_;
    te_retry_us_ = esp_timer_get_time() + 1000000;
    if (timeouts == 1 || timeouts % 30 == 0)
        ESP_LOGW(kTag, "TE wait timed out; refresh continues (timeouts=%lu)",
                 static_cast<unsigned long>(timeouts));
}

void EspMosaicoHardware::RoundDisplayArea(lv_area_t* area) {
    mosaico_board::RoundDisplayArea(area);
}

bool EspMosaicoHardware::OnColorTransferDone(
    esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    BaseType_t awakened = pdFALSE;
    xSemaphoreGiveFromISR(self->transfer_done_, &awakened);
    return awakened == pdTRUE;
}

void EspMosaicoHardware::FlushDisplay(lv_display_t* display,
                                      const lv_area_t* area, uint8_t* pixels) {
    auto* self = static_cast<EspMosaicoHardware*>(lv_display_get_driver_data(display));
    const lv_draw_buf_t* active_buffer = lv_display_get_buf_active(display);
    if (self == nullptr || area == nullptr || pixels == nullptr ||
        !mosaico_board::ValidDisplayArea(*area) || active_buffer == nullptr) {
        lv_display_flush_ready(display);
        return;
    }
    const size_t width = lv_area_get_width(area);
    const size_t row_bytes = width * sizeof(uint16_t);
    const uint32_t source_stride = active_buffer->header.stride;
    if (source_stride < row_bytes || source_stride % 2 != 0) {
        self->redraw_pending_ = true;
        lv_display_flush_ready(display);
        return;
    }
    // UI-only changes also use the board compositor when an opaque fullscreen
    // game and valid alpha planes cover everything. Decide once per refresh:
    // each LVGL chunk belongs to that same complete prepared frame. This avoids
    // restoring/copying 460,800 bytes for a small Back/volume/status-bar change.
    if (!self->flush_direct_checked_) {
        self->flush_direct_checked_ = true;
        self->flush_direct_ready_ = self->PrepareDirectFrame(&self->flush_direct_request_);
    }
    if (self->flush_direct_ready_) {
        if (lv_display_flush_is_last(display)) {
            lv_area_t unused;
            (void)self->pending_rows_.Take(&unused);
            self->compose_failed_ = false;
            const auto& request = self->flush_direct_request_;
            (void)self->QueuePresent(request.area, request.timestamp,
                request.frame_id, request.raster_ready_tick_us);
        }
        lv_display_flush_ready(display);
        return;
    }
    const uint64_t timestamp = mosaico_pxa_surface::ComposeFlushArea(
        area, pixels, source_stride);
    const bool last = lv_display_flush_is_last(display);
    if (last) pxa_board_performance_note_frame();
    // Paint the diagnostics before transferring ownership to the worker.
    pxa_board_performance_draw_rgb565(reinterpret_cast<uint16_t*>(pixels),
        mosaico_board::kWidth, mosaico_board::kHeight, source_stride / 2,
        area->x1, area->y1, width, lv_area_get_height(area), true);
    if (!self->present_pixels_current_) {
        // Full game frames exchange buffers. Restore the newest complete
        // image once before merging UI patches into the producer again.
        const lv_area_t full = {0, 0, mosaico_board::kWidth - 1, mosaico_board::kHeight - 1};
        self->WaitForFlush();
        mosaico_board::CopyRefreshRows(full, self->scanout_pixels_, self->present_pixels_);
        self->present_pixels_current_ = true;
    }
    if (!self->pending_rows_.Merge(*area, pixels, source_stride,
                                   self->present_pixels_)) {
        self->compose_failed_ = true;
        self->redraw_pending_ = true;
    }
    if (last) {
        lv_area_t rows;
        if (self->pending_rows_.Take(&rows) && !self->compose_failed_)
            (void)self->QueuePresent(rows, timestamp);
        self->compose_failed_ = false;
    }
    lv_display_flush_ready(display);
}

bool EspMosaicoHardware::TransferArea(
    const lv_area_t& area, const uint8_t* pixels, uint32_t source_stride,
    bool last, uint64_t timestamp, const char* source_name, uint64_t frame_id) {
    if (pixels == nullptr || !mosaico_board::ValidDisplayArea(area) ||
        source_stride < static_cast<uint32_t>(lv_area_get_width(&area) * 2) ||
        source_stride % 2 != 0 || !WaitForTransfer(pdMS_TO_TICKS(10))) {
        displayed_frame_valid_ = false;
        displayed_frame_initialized_ = false;
        refresh_failed_ = true;
        return false;
    }
    const int64_t te_wait_started = esp_timer_get_time();
    WaitForTe(area);
    te_wait_us_ = esp_timer_get_time() - te_wait_started;
    const int64_t transfer_started_us = esp_timer_get_time();
    // Set the full refresh window once. RAMWRC continues at the next pixel;
    // per-band CASET/RASET would drain SPI and add four polling transactions.
    const uint8_t columns[] = {static_cast<uint8_t>(area.x1 >> 8),
        static_cast<uint8_t>(area.x1), static_cast<uint8_t>(area.x2 >> 8),
        static_cast<uint8_t>(area.x2)};
    const uint8_t rows[] = {static_cast<uint8_t>(area.y1 >> 8),
        static_cast<uint8_t>(area.y1), static_cast<uint8_t>(area.y2 >> 8),
        static_cast<uint8_t>(area.y2)};
    if (esp_lcd_panel_io_tx_param(panel_io_, 0x02002a00, columns, sizeof(columns)) != ESP_OK ||
        esp_lcd_panel_io_tx_param(panel_io_, 0x02002b00, rows, sizeof(rows)) != ESP_OK) {
        ++transfer_errors_;
        displayed_frame_valid_ = false;
        displayed_frame_initialized_ = false;
        redraw_pending_ = true;
        return false;
    }
    const bool completed = mosaico_board::TransferBands(
        area, pixels, source_stride, transfer_pixels_,
        [this]() {
            if (WaitForTransfer(pdMS_TO_TICKS(1000))) return true;
            ++transfer_errors_;
            ESP_LOGE(kTag, "Panel transfer timed out; retaining the DMA buffers");
            return false;
        },
        [this, &area](int32_t first_row, int32_t rows, const uint8_t* staging) {
            (void)xSemaphoreTake(transfer_done_, 0);
            const int command = first_row == area.y1 ? 0x32002c00 : 0x32003c00;
            const esp_err_t result = esp_lcd_panel_io_tx_color(
                panel_io_, command, staging,
                lv_area_get_width(&area) * rows * sizeof(uint16_t));
            if (result != ESP_OK) {
                ++transfer_errors_;
                ESP_LOGE(kTag, "Panel transfer failed: %s", esp_err_to_name(result));
                return false;
            }
            transfer_pending_ = true;
            return true;
        },
        [](int32_t, int32_t) {});
    const uint32_t transfer_us = static_cast<uint32_t>(
        esp_timer_get_time() - transfer_started_us);
    transfer_duration_us_ = transfer_us;
    transfer_max_us_ = std::max(transfer_max_us_.load(), transfer_us);
    if (completed && scan_deadline_us_ != 0 &&
        esp_timer_get_time() >= scan_deadline_us_)
        ++scan_overruns_;
    if (!completed) {
        displayed_frame_valid_ = false;
        displayed_frame_initialized_ = false;
        refresh_failed_ = true;
    } else if (area.x1 == 0 && area.y1 == 0 &&
               area.x2 == mosaico_board::kWidth - 1 &&
               area.y2 == mosaico_board::kHeight - 1) {
        displayed_frame_initialized_ = true;
    }
    if (last) {
        if (completed && !refresh_failed_ && displayed_frame_initialized_) {
            ++completed_frames_;
            displayed_frame_valid_ = true;
            displayed_frame_id_ = frame_id != 0 ? frame_id : completed_frames_.load();
            displayed_timestamp_us_ = esp_timer_get_time();
            displayed_source_ = source_name;
            if (frame_id == 0)
                pxa_esp_surface_note_frame_presented(timestamp, displayed_timestamp_us_);
        } else {
            redraw_pending_ = true;
        }
        refresh_failed_ = false;
    }
    return completed;
}

bool EspMosaicoHardware::PresentDirectFrame() {
    FlushRequest request;
    return PrepareDirectFrame(&request) && QueuePresent(request.area,
        request.timestamp, request.frame_id, request.raster_ready_tick_us);
}

bool EspMosaicoHardware::PrepareDirectFrame(FlushRequest* request) {
    if (!panel_on_.load() || reference_ui_ == nullptr ||
        pxsys_reference_lvgl_is_locked(reference_ui_)) return false;
    pxa_esp_surface_frame_t frame;
    if (!pxa_esp_surface_acquire_prepared_with_alpha(&frame)) return false;
    direct_raster_us_ = mosaico_pxa_surface::RasterTimeUs();
    if (!mosaico_pxa_surface::DirectFrameEligible(frame)) {
        pxa_esp_surface_release_frame(frame.lease);
        return false;
    }
    const lv_area_t area = {0, 0, mosaico_board::kWidth - 1, mosaico_board::kHeight - 1};
    const int64_t compose_started = esp_timer_get_time();
    if (mosaico_pxa_surface::TryAcceleratedDirectFrame(frame, present_pixels_)) {
        ++direct_ppa_frames_;
    } else {
        (void)mosaico_pxa_surface::ComposeFrame(frame, &area, present_pixels_);
        ++direct_cpu_frames_;
    }
    if (frame.ui_alpha_plane.visible || frame.system_alpha_plane.visible)
        ++direct_alpha_frames_;
    direct_compose_us_ = esp_timer_get_time() - compose_started;
    const uint64_t timestamp = frame.input_timestamp_us;
    const uint64_t frame_id = frame.frame_id;
    const uint32_t raster_ready_tick_us = frame.raster_ready_tick_us;
    pxa_esp_surface_release_frame(frame.lease);
    pxa_board_performance_note_frame();
    pxa_board_performance_draw_rgb565(reinterpret_cast<uint16_t*>(present_pixels_),
        mosaico_board::kWidth, mosaico_board::kHeight, mosaico_board::kWidth,
        0, 0, mosaico_board::kWidth, mosaico_board::kHeight, true);
    *request = {area, timestamp, frame_id, raster_ready_tick_us};
    return true;
}

void EspMosaicoHardware::WaitForFlush() {
    // Called with the display lock: no producer can enqueue another frame
    // while snapshots, panel controls or initial display-on wait for completion.
    xSemaphoreTake(present_available_, portMAX_DELAY);
    xSemaphoreGive(present_available_);
}

bool EspMosaicoHardware::QueuePresent(const lv_area_t& area, uint64_t timestamp,
                                      uint64_t frame_id, uint32_t raster_ready_tick_us) {
    // Exactly one submitted frame, plus the frame LVGL is currently drawing.
    // Backpressure prevents a backlog of stale scroll frames and lost patches.
    const int64_t wait_started = esp_timer_get_time();
    xSemaphoreTake(present_available_, portMAX_DELAY);
    present_wait_us_ = esp_timer_get_time() - wait_started;
    refresh_wait_us_ += present_wait_us_.load();
    const int64_t snapshot_started = esp_timer_get_time();
    const bool exchanged = frame_id != 0;
    const bool was_current = present_pixels_current_;
    const bool snapshotted = exchanged
        ? mosaico_board::ExchangeRefreshFrame(area, present_pixels_, scanout_pixels_)
        : mosaico_board::CopyRefreshRows(area, present_pixels_, scanout_pixels_);
    if (!snapshotted) {
        xSemaphoreGive(present_available_);
        redraw_pending_ = true;
        return false;
    }
    if (exchanged) present_pixels_current_ = false;
    snapshot_us_ = esp_timer_get_time() - snapshot_started;
    const FlushRequest request = {area, timestamp, frame_id, raster_ready_tick_us};
    if (xQueueSend(flush_queue_, &request, 0) == pdTRUE) return true;
    if (exchanged) {
        std::swap(present_pixels_, scanout_pixels_);
        present_pixels_current_ = was_current;
    }
    xSemaphoreGive(present_available_);
    redraw_pending_ = true;
    return false;
}

void EspMosaicoHardware::FlushTask(void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    FlushRequest request;
    for (;;) {
        if (xQueueReceive(self->flush_queue_, &request, portMAX_DELAY) != pdTRUE)
            continue;
        xSemaphoreTake(self->panel_mutex_, portMAX_DELAY);
        const uint8_t* pixels = self->scanout_pixels_ +
            request.area.y1 * mosaico_board::kWidth * sizeof(uint16_t);
        const bool completed = self->TransferArea(request.area, pixels,
            mosaico_board::kWidth * sizeof(uint16_t), true,
            request.timestamp, request.frame_id ? "surface-direct" : "lvgl-composited",
            request.frame_id);
        if (completed && request.frame_id) {
            ++self->direct_frames_;
            pxa_esp_surface_note_game_frame_presented(request.frame_id, self->displayed_timestamp_us_);
            if (request.raster_ready_tick_us) {
                pxa_esp_surface_note_raster_frame_presented(request.frame_id, request.timestamp,
                    self->displayed_timestamp_us_,
                    static_cast<uint32_t>(self->displayed_timestamp_us_) - request.raster_ready_tick_us);
            } else pxa_esp_surface_note_frame_presented(request.timestamp, self->displayed_timestamp_us_);
        }
        xSemaphoreGive(self->panel_mutex_);
        xSemaphoreGive(self->present_available_);
    }
}

bool EspMosaicoHardware::WaitForTransfer(TickType_t timeout) {
    if (!transfer_pending_) return true;
    if (xSemaphoreTake(transfer_done_, timeout) != pdTRUE) return false;
    transfer_pending_ = false;
    return true;
}

bool EspMosaicoHardware::InitializeTouch() {
    esp_lcd_panel_io_i2c_config_t io_config = {};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST92XX_ADDRESS;
    io_config.scl_speed_hz = 400000;
    io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = true;
    io_config.transaction_timeout_ms = 20;
    if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io_) != ESP_OK)
        return false;
    esp_lcd_touch_config_t config = {};
    config.x_max = mosaico_board::kWidth - 1;
    config.y_max = mosaico_board::kHeight - 1;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = static_cast<gpio_num_t>(mosaico_board::kTouchInterrupt);
    if (esp_lcd_touch_new_i2c_cst92xx(touch_io_, &config, &touch_) != ESP_OK)
        return false;
    if (esp_lv_adapter_lock(1000) != ESP_OK) return false;
    lv_indev_t** inputs[] = {&touch_input_, &secondary_touch_input_};
    for (size_t slot = 0; slot < EspMosaicoTouchState::kMaxPoints; ++slot) {
        *inputs[slot] = lv_indev_create();
        if (*inputs[slot] == nullptr) break;
        lv_indev_set_type(*inputs[slot], LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(*inputs[slot], display_);
        lv_indev_set_user_data(*inputs[slot], &touch_contexts_[slot]);
        lv_indev_set_read_cb(*inputs[slot], ReadTouch);
        lv_indev_set_mode(*inputs[slot], LV_INDEV_MODE_TIMER);
        lv_timer_set_period(lv_indev_get_read_timer(*inputs[slot]), 6);
    }
    esp_lv_adapter_unlock();
    if (touch_input_ == nullptr || secondary_touch_input_ == nullptr) {
        if (esp_lv_adapter_lock(1000) == ESP_OK) {
            if (touch_input_) lv_indev_delete(touch_input_);
            if (secondary_touch_input_) lv_indev_delete(secondary_touch_input_);
            touch_input_ = secondary_touch_input_ = nullptr;
            esp_lv_adapter_unlock();
        }
        return false;
    }
    if (xTaskCreate(PollTouch, "mosaico_touch", 4096, this, 6,
                    &touch_task_) != pdPASS) {
        if (esp_lv_adapter_lock(1000) == ESP_OK) {
            lv_indev_delete(touch_input_);
            lv_indev_delete(secondary_touch_input_);
            touch_input_ = secondary_touch_input_ = nullptr;
            esp_lv_adapter_unlock();
        }
        return false;
    }
    ESP_LOGI(kTag, "CST92xx two-contact polling ready on GPIO%d", mosaico_board::kTouchInterrupt);
    return true;
}

void EspMosaicoHardware::PollTouch(void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    for (;;) {
        esp_lcd_touch_point_data_t raw[EspMosaicoTouchState::kMaxPoints] = {};
        uint8_t count = 0;
        esp_err_t status = esp_lcd_touch_read_data(self->touch_);
        if (status == ESP_OK)
            status = esp_lcd_touch_get_data(
                self->touch_, raw, &count, EspMosaicoTouchState::kMaxPoints);
        self->touch_reads_.fetch_add(1);
        self->touch_last_status_.store(status);
        if (status != ESP_OK) self->touch_errors_.fetch_add(1);
        EspMosaicoTouchPoint points[EspMosaicoTouchState::kMaxPoints] = {};
        count = std::min<uint8_t>(count, EspMosaicoTouchState::kMaxPoints);
        for (uint8_t index = 0; index < count; ++index) {
            points[index].x = raw[index].x;
            points[index].y = raw[index].y;
            points[index].id = raw[index].track_id;
        }
        const int64_t timestamp_us = esp_timer_get_time();
        portENTER_CRITICAL(&self->touch_lock_);
        self->touch_state_.Update(status == ESP_OK, points, count, timestamp_us);
        const size_t active = self->touch_state_.Snapshot(timestamp_us, points,
            EspMosaicoTouchState::kMaxPoints);
        self->touch_slots_.Update(points, active);
        for (size_t slot = 0; slot < EspMosaicoTouchState::kMaxPoints; ++slot)
            self->touch_queue_[slot].Update(self->touch_slots_.Get(slot), timestamp_us);
        const bool pressed = status == ESP_OK && active > 0;
        portEXIT_CRITICAL(&self->touch_lock_);
        if (pressed) self->touch_reports_.fetch_add(1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void EspMosaicoHardware::ReadTouch(lv_indev_t* input, lv_indev_data_t* data) {
    auto* context = static_cast<TouchInputContext*>(lv_indev_get_user_data(input));
    auto* self = context->owner;
    // Each indev's ordinal becomes a
    // distinct Guest pointer ID through the existing LVGL UI adapter, which
    // also preserves hit testing, trusted overlays and logical coordinates.
    if (context->slot == 0 && self->panel_on_ && !self->wake_touch_.active()) {
        EspMosaicoTouchPoint points[EspMosaicoTouchState::kMaxPoints] = {};
        const int64_t timestamp_us = esp_timer_get_time();
        portENTER_CRITICAL(&self->touch_lock_);
        const size_t count = self->touch_state_.Snapshot(
            timestamp_us, points, EspMosaicoTouchState::kMaxPoints);
        portEXIT_CRITICAL(&self->touch_lock_);
        EspMosaicoTouchEvent events[EspMosaicoTouchEvents::kMaxEvents] = {};
        const size_t event_count = self->touch_events_.Update(points, count, events);
#if CONFIG_LV_USE_GESTURE_RECOGNITION
        lv_indev_touch_data_t gestures[EspMosaicoTouchEvents::kMaxEvents] = {};
        for (size_t index = 0; index < event_count; ++index) {
            gestures[index].state = events[index].pressed
                ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            gestures[index].point.x = events[index].point.x;
            gestures[index].point.y = events[index].point.y;
            gestures[index].id = events[index].point.id;
            gestures[index].timestamp = lv_tick_get();
        }
        lv_indev_gesture_recognizers_update(input, gestures, event_count);
#else
        (void)event_count;
#endif
    }
    portENTER_CRITICAL(&self->touch_lock_);
    const bool queued = self->touch_queue_[context->slot].pending();
    const auto sample = self->touch_queue_[context->slot].Read();
    data->continue_reading = self->touch_queue_[context->slot].pending();
    portEXIT_CRITICAL(&self->touch_lock_);
    data->state = sample.event.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = sample.event.point.x;
    data->point.y = sample.event.point.y;
    data->timestamp = MosaicoTouchLvglTimestamp(sample.timestamp_us,
        esp_timer_get_time(), lv_tick_get(), queued);
#if CONFIG_LV_USE_GESTURE_RECOGNITION
    if (context->slot == 0) lv_indev_gesture_recognizers_set_data(input, data);
#endif
    const bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    const bool asleep = !self->panel_on_.load();
    if (pressed && asleep) {
        lv_display_trigger_activity(self->display_);
        self->wake_pending_ = true;
    }
    if (self->wake_touch_.Consume(context->slot, pressed, asleep))
        data->state = LV_INDEV_STATE_RELEASED;
}

bool EspMosaicoHardware::InitializeButton() {
    const button_config_t config = {.long_press_time = 1200};
    const button_gpio_config_t gpio_config = {
        .gpio_num = mosaico_board::kFunctionButton,
        .active_level = 0,
    };
    if (iot_button_new_gpio_device(&config, &gpio_config, &button_) != ESP_OK)
        return false;
    if (iot_button_register_cb(button_, BUTTON_SINGLE_CLICK, nullptr,
        [](void*, void* context) {
            static_cast<EspMosaicoHardware*>(context)->home_pending_.store(true);
        }, this) != ESP_OK)
        return false;
    return iot_button_register_cb(button_, BUTTON_LONG_PRESS_START, nullptr,
        [](void*, void* context) {
            static_cast<EspMosaicoHardware*>(context)->provisioning_pending_.store(true);
        }, this) == ESP_OK;
}

bool EspMosaicoHardware::InitializeWifi() {
    // The board's Chinese/Singapore display profile uses UTC+8. SNTP still
    // sets UTC epoch time; TZ affects only local calendar/status formatting.
    setenv("TZ", "CST-8", 1);
    tzset();
    WifiManagerConfig config;
    config.ssid_prefix = "ESP-Mosaico";
    config.language = "zh-CN";
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected && time_sync_initialized_.load()) {
            const esp_err_t result = esp_netif_sntp_start();
            if (result != ESP_OK)
                ESP_LOGW(kTag, "Unable to start time sync: %s", esp_err_to_name(result));
        }
    });
    if (!wifi.Initialize(config)) return false;
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time_config.start = false;
    time_config.wait_for_sync = false;
    time_config.sync_cb = [](struct timeval*) {
        ESP_LOGI(kTag, "System time synchronized");
    };
    const esp_err_t time_result = esp_netif_sntp_init(&time_config);
    time_sync_initialized_ = time_result == ESP_OK;
    if (time_result != ESP_OK)
        ESP_LOGW(kTag, "Time sync unavailable: %s", esp_err_to_name(time_result));
    wifi.StartStation();
    return true;
}

void EspMosaicoHardware::AttachSystem(
    pxsys_standard_system_t* system, pxsys_reference_lvgl_t* reference_ui) {
    system_ = system;
    reference_ui_ = reference_ui;
    if (boot_screen_ != nullptr) {
        lv_obj_delete(boot_screen_);
        boot_screen_ = nullptr;
    }
    lv_display_trigger_activity(display_);
    if (reference_ui_ != nullptr)
        (void)pxsys_reference_lvgl_set_lock_changed_callback(
            reference_ui_, this, [](void* context, bool locked) {
                auto* self = static_cast<EspMosaicoHardware*>(context);
                pxa_esp_surface_set_display_unlocked(!locked);
                (void)pxa_host_set_display_interactive(!locked);
                if (!locked) {
                    // Defer redraw until the lock callback and input event
                    // have finished, so composition sees the unlocked state.
                    self->wake_pending_ = true;
                }
            });
    (void)lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<EspMosaicoHardware*>(lv_timer_get_user_data(timer));
        if (self->home_pending_.exchange(false)) {
            lv_display_trigger_activity(self->display_);
            if (!self->panel_on_ || (self->reference_ui_ != nullptr &&
                pxsys_reference_lvgl_is_locked(self->reference_ui_))) {
                self->wake_pending_ = true;
            } else if (self->reference_ui_ != nullptr) {
                (void)pxsys_reference_lvgl_home(self->reference_ui_);
            }
        }
        if (self->provisioning_pending_.exchange(false) && self->wifi_initialized_) {
            self->wifi_enabled_ = true;
            WifiManager::GetInstance().StartConfigAp();
        }
        if (self->back_pending_.exchange(false) && self->system_ != nullptr) {
            const bool dismissed = self->reference_ui_ != nullptr &&
                pxsys_reference_lvgl_dismiss_overlay(self->reference_ui_);
            if (!dismissed) {
                pxsys_back_result_t result;
                (void)pxsys_task_manager_back(
                    pxsys_standard_system_tasks(self->system_), &result);
            }
        }
        if (self->wake_pending_.exchange(false)) self->WakePanel();
        self->PublishStatus();
        self->PublishDiagnostics();
    }, 100, this);
    PublishStatus();
}

bool EspMosaicoHardware::SetWifiEnabled(bool enabled) {
    if (!wifi_initialized_) return false;
    if (wifi_enabled_.exchange(enabled) == enabled) return true;
    auto& wifi = WifiManager::GetInstance();
    if (enabled) {
        wifi.StartStation();
    } else {
        wifi.StopConfigAp();
        wifi.StopStation();
    }
    return true;
}

bool EspMosaicoHardware::SetVolume(uint8_t percent) {
    if (!audio_initialized_) return false;
    return audio_.SetVolume(percent);
}

bool EspMosaicoHardware::SetBrightness(uint8_t percent) {
    brightness_ = std::min<uint8_t>(percent, 100);
    return ApplyBrightness();
}

bool EspMosaicoHardware::ApplyBrightness() {
    if (panel_ == nullptr || esp_lv_adapter_lock(1000) != ESP_OK) return false;
    const uint8_t logical_percent = idle_dim_enabled_.load()
        ? std::min(brightness_.load(), idle_dim_percent_.load())
        : brightness_.load();
    const uint8_t panel_percent = mosaico_board::PanelBrightnessPercent(logical_percent);
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    const esp_err_t result = esp_lcd_panel_co5300_set_brightness(panel_, panel_percent);
    xSemaphoreGive(panel_mutex_);
    esp_lv_adapter_unlock();
    if (result == ESP_OK)
        ESP_LOGI(kTag, "Brightness logical=%u%% panel=%u%% dim=%d",
                 static_cast<unsigned>(logical_percent),
                 static_cast<unsigned>(panel_percent), idle_dim_enabled_.load());
    return result == ESP_OK;
}

void EspMosaicoHardware::SetIdleDim(bool enabled, uint8_t percent) {
    idle_dim_enabled_ = enabled;
    idle_dim_percent_ = std::min<uint8_t>(percent, 100);
    (void)ApplyBrightness();
    if (!enabled && !panel_on_) wake_pending_ = true;
}

void EspMosaicoHardware::IdleScreenOff() {
    wake_pending_ = false;
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    if (panel_on_ && esp_lcd_panel_disp_on_off(panel_, false) == ESP_OK)
        panel_on_ = false;
    xSemaphoreGive(panel_mutex_);
}

void EspMosaicoHardware::ShowBootFrame() {
    if (esp_lv_adapter_lock(2000) != ESP_OK) return;
    lv_lock();
    boot_screen_ = lv_obj_create(lv_display_get_screen_active(display_));
    if (boot_screen_ != nullptr) {
        lv_obj_remove_style_all(boot_screen_);
        lv_obj_set_size(boot_screen_, mosaico_board::kWidth, mosaico_board::kHeight);
        lv_obj_set_style_bg_color(boot_screen_, lv_color_hex(0x111827), 0);
        lv_obj_set_style_bg_opa(boot_screen_, LV_OPA_COVER, 0);
        lv_obj_t* label = lv_label_create(boot_screen_);
        lv_label_set_text(label, "PXA\nStarting...");
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xe5e7eb), 0);
        lv_obj_center(label);
        ShowInitialFrame();
        ESP_LOGI(kTag, "Boot frame visible at %lld ms", esp_timer_get_time() / 1000);
    }
    lv_unlock();
    esp_lv_adapter_unlock();
}

void EspMosaicoHardware::WakePanel() {
    // This runs on the UI timer, never inside an input/lock-change callback.
    // Preserve the lock screen; waking is separate from its unlock gesture.
    lv_display_trigger_activity(display_);
    idle_dim_enabled_ = false;
    (void)ApplyBrightness();
    ShowInitialFrame();
    ESP_LOGI(kTag, "Panel wake: locked=%d", reference_ui_ != nullptr &&
        pxsys_reference_lvgl_is_locked(reference_ui_));
}

void EspMosaicoHardware::ShowInitialFrame() {
    if (esp_lv_adapter_lock(2000) != ESP_OK) return;
    lv_lock();
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    lv_refr_now(display_);
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    if (displayed_frame_valid_)
        panel_on_ = esp_lcd_panel_disp_on_off(panel_, true) == ESP_OK;
    xSemaphoreGive(panel_mutex_);
    lv_unlock();
    esp_lv_adapter_unlock();
}

bool EspMosaicoHardware::CaptureDisplayedRgb565(uint16_t* pixels,
                                                size_t pixel_count) {
    if (pixels == nullptr || pixel_count < kFramePixels ||
        esp_lv_adapter_lock(1000) != ESP_OK)
        return false;
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    const bool valid = displayed_frame_valid_;
    if (valid) mosaico_board::CaptureScanoutRgb565(scanout_pixels_, pixels);
    xSemaphoreGive(panel_mutex_);
    esp_lv_adapter_unlock();
    return valid;
}

void EspMosaicoHardware::PublishStatus() {
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
    auto& wifi = WifiManager::GetInstance();
    const bool connected = wifi_initialized_ && wifi_enabled_.load() && wifi.IsConnected();
    status.network_connected = connected;
    status.network_type = connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    status.network_signal_level = connected ? SignalLevel(wifi.GetRssi()) : 0;
    status.wifi_supported = wifi_initialized_;
    status.wifi_enabled = wifi_initialized_ && wifi_enabled_.load();
    status.wifi_connected = connected;
    status.wifi_signal_level = status.network_signal_level;
    status.volume_supported = audio_initialized_;
    status.volume_percent = audio_.volume();
    status.brightness_supported = 1;
    status.brightness_percent = brightness_.load();
    (void)pxsys_system_status_service_update(
        pxsys_standard_system_status(system_), &status);
}

void EspMosaicoHardware::PublishDiagnostics() {
    const int64_t timestamp_us = esp_timer_get_time();
    if (timestamp_us - diagnostics_logged_us_ < 5000000) return;
    const uint64_t fps_tenths = static_cast<uint64_t>(
        completed_frames_ - diagnostics_logged_frames_) * 10000000 /
        static_cast<uint64_t>(timestamp_us - diagnostics_logged_us_);
    diagnostics_logged_frames_ = completed_frames_;
    diagnostics_logged_us_ = timestamp_us;
    audio_.PublishDiagnostics();
    EspMosaicoTouchPoint points[EspMosaicoTouchState::kMaxPoints] = {};
    portENTER_CRITICAL(&touch_lock_);
    const size_t count = touch_state_.Snapshot(
        timestamp_us, points, EspMosaicoTouchState::kMaxPoints);
    portEXIT_CRITICAL(&touch_lock_);
    ESP_LOGI(kTag, "health reset=%d uptime=%llds frames=%lu fps=%u.%u direct=%lu spi_errors=%lu "
             "touch_ready=%d touch_reads=%lu touch_reports=%lu touch_errors=%lu "
             "touch_status=%s points=%u xy=%u,%u te_edges=%lu te_period_us=%lu te_timeouts=%lu "
             "te_blank_us=%lu tx_us=%lu tx_max_us=%lu scan_overruns=%lu sram=%u",
             static_cast<int>(esp_reset_reason()), timestamp_us / 1000000,
             static_cast<unsigned long>(completed_frames_),
             static_cast<unsigned>(fps_tenths / 10),
             static_cast<unsigned>(fps_tenths % 10),
             static_cast<unsigned long>(direct_frames_),
             static_cast<unsigned long>(transfer_errors_),
             touch_task_ != nullptr,
             static_cast<unsigned long>(touch_reads_.load()),
             static_cast<unsigned long>(touch_reports_.load()),
             static_cast<unsigned long>(touch_errors_.load()),
             esp_err_to_name(touch_last_status_.load()),
             static_cast<unsigned>(count),
             static_cast<unsigned>(points[0].x),
             static_cast<unsigned>(points[0].y),
             static_cast<unsigned long>(te_edges_.load()),
             static_cast<unsigned long>(te_period_us_.load()),
             static_cast<unsigned long>(te_timeouts_.load()),
             static_cast<unsigned long>(te_blank_us_.load()),
             static_cast<unsigned long>(transfer_duration_us_.load()),
             static_cast<unsigned long>(transfer_max_us_.load()),
             static_cast<unsigned long>(scan_overruns_.load()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    // Keep this separate so every field fits PXADB's 320-byte log payload.
    ESP_LOGI(kTag, "frame_stats target_fps=30 pipeline=async fps=%u.%u "
             "render_us=%lu render_max_us=%lu present_wait_us=%lu te_wait_us=%lu "
             "tx_us=%lu tx_max_us=%lu scan_overruns=%lu time_valid=%d sram=%u",
             static_cast<unsigned>(fps_tenths / 10),
             static_cast<unsigned>(fps_tenths % 10),
             static_cast<unsigned long>(render_duration_us_.load()),
             static_cast<unsigned long>(render_max_us_.load()),
             static_cast<unsigned long>(present_wait_us_.load()),
             static_cast<unsigned long>(te_wait_us_.load()),
             static_cast<unsigned long>(transfer_duration_us_.load()),
             static_cast<unsigned long>(transfer_max_us_.load()),
             static_cast<unsigned long>(scan_overruns_.load()),
             std::time(nullptr) >= 1577836800,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    ESP_LOGI(kTag, "power idle_ms=%lu locked=%d panel_on=%d dim=%d touch_clock=lvgl",
             static_cast<unsigned long>(lv_display_get_inactive_time(display_)),
             reference_ui_ != nullptr && pxsys_reference_lvgl_is_locked(reference_ui_),
             panel_on_.load(), idle_dim_enabled_.load());
    ESP_LOGI(kTag, "surface_stats presenter_core=1 raster_us=%lu compose_us=%lu snapshot_us=%lu ppa_frames=%lu cpu_frames=%lu alpha_frames=%lu ppa_background=%lu",
             static_cast<unsigned long>(direct_raster_us_.load()),
             static_cast<unsigned long>(direct_compose_us_.load()),
             static_cast<unsigned long>(snapshot_us_.load()),
             static_cast<unsigned long>(direct_ppa_frames_.load()),
             static_cast<unsigned long>(direct_cpu_frames_.load()),
             static_cast<unsigned long>(direct_alpha_frames_.load()),
             static_cast<unsigned long>(mosaico_pxa_surface::AcceleratedFillCount()));
}
