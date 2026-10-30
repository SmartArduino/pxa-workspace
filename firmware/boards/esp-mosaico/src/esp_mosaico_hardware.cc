#include "esp_mosaico_hardware.h"
#include "esp_mosaico_transfer.h"
#include "esp_mosaico_refresh.h"
#include "esp_mosaico_storage.h"
#include "esp_mosaico_sensors.h"
#include "esp_mosaico_haptics.h"
#include "esp_mosaico_peripherals.h"

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
#include <esp_rom_sys.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <button_gpio.h>
#include "esp_mosaico_input_lvgl.h"
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_console.h>
#include <tinyusb_default_config.h>
#include <wifi_manager.h>

#include "esp_mosaico_pxa_surface.h"

#ifndef CONFIG_PXA_MOSAICO_LCD_PSRAM_DMA_DIRECT
#define CONFIG_PXA_MOSAICO_LCD_PSRAM_DMA_DIRECT 0
#endif

namespace {

constexpr char kTag[] = "mosaico_hw";
constexpr size_t kFramePixels = mosaico_board::kWidth * mosaico_board::kHeight;
constexpr bool kDirectPsramDma = CONFIG_PXA_MOSAICO_LCD_PSRAM_DMA_DIRECT;

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
    usb_console_ready_ = InitializeUsb();
    if (!usb_console_ready_) ESP_LOGW(kTag, "USB CDC console unavailable");
    if (!DetectHardware() || !InitializePower() || !InitializeI2c() ||
        !InitializeDisplay())
        return false;
    ShowBootFrame();
    if (!EspMosaicoInitializeHaptics()) ESP_LOGW(kTag, "Vibration motor unavailable");
    EspMosaicoInitializeSensors(i2c_bus_, &battery_);
    if (!EspMosaicoInitializeStorage()) ESP_LOGW(kTag, "Continuing without NAND storage");
    if (!InitializeTouch()) ESP_LOGW(kTag, "Continuing without CST92xx touch");
    // The same worker owns touch and low-rate battery I/O, even if the touch
    // controller is absent. Never poll hardware from the LVGL status callback.
    if (xTaskCreate(PollTouch, "mosaico_input", 4096, this, 6,
                    &touch_task_) != pdPASS)
        ESP_LOGW(kTag, "Touch/battery polling task unavailable");
    if (!InitializeButton()) ESP_LOGW(kTag, "Function button unavailable");
    if (!InitializeBootButton()) ESP_LOGW(kTag, "BOOT/Back button unavailable");
    if (!EspMosaicoInitializePeripherals(i2c_bus_, hardware_revision_, usb_console_ready_))
        ESP_LOGW(kTag, "Expansion interface unavailable");
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
    hardware_revision_ = version;
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
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) return false;
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
        .flags = {.quad_mode = true, .psram_dma_direct = kDirectPsramDma},
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
    // Input timers must win the lock handoff over the continuously notified
    // priority-5 game presenter; equal priority can let it reacquire the
    // recursive adapter mutex before the sleeping LVGL worker runs.
    adapter_config.task_priority = 6;
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
    display_config.profile.buffer_height = mosaico_board::kDrawBufferRows;
    display_config.profile.use_psram = true;
    display_config.profile.require_double_buffer = false;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.te_sync.gpio_num = -1;
    if (esp_lv_adapter_lock(1000) != ESP_OK) return false;
    display_ = esp_lv_adapter_register_display(&display_config);
    if (display_ == nullptr) {
        esp_lv_adapter_unlock();
        return false;
    }
    lv_display_set_driver_data(display_, this);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display_, FlushDisplay);
    // The worker converts the final dirty snapshot outside the UI lock.
    // LVGL must retain the source until that conversion calls flush_ready.
    lv_display_set_flush_wait_cb(display_, [](lv_display_t* display) {
        static_cast<EspMosaicoHardware*>(lv_display_get_driver_data(display))->WaitForUiSnapshot();
    });
    lv_timer_set_period(lv_display_get_refr_timer(display_), mosaico_board::kUiRefreshPeriodMs);
    lv_display_add_event_cb(display_, [](lv_event_t* event) {
        auto* self = static_cast<EspMosaicoHardware*>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_REFR_START) {
            // UI priority can win the lock before the asynchronous presenter.
            // Compose the complete eligible frame here, before restoring and
            // software-rendering a retained framebuffer that would be discarded.
            (void)mosaico_board::PresentPendingRefresh(self->display_, [self]() {
                return self->PresentDirectFrame();
            });
        } else if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
            self->refresh_started_us_ = esp_timer_get_time();
            self->WaitForUiSnapshot();
            self->refresh_wait_us_ = esp_timer_get_time() - self->refresh_started_us_;
            if (self->ui_frame_state_.load(std::memory_order_acquire) == UiFrameState::RestoreRequired) {
                // Restore before drawing, so a direct game does not leave old
                // UI pixels around newly drawn patches. Never overwrite a
                // patch which LVGL has already rendered in this refresh.
                const int64_t started = esp_timer_get_time();
                self->WaitForFlush();
                xSemaphoreTake(self->panel_mutex_, portMAX_DELAY);
                self->refresh_wait_us_ += esp_timer_get_time() - started;
                const lv_area_t full = {0, 0, mosaico_board::kWidth - 1, mosaico_board::kHeight - 1};
                mosaico_board::CopyRefreshArea(full, self->scanout_pixels_,
                    lv_display_get_buf_active(self->display_)->data, true);
                xSemaphoreGive(self->panel_mutex_);
                self->ui_frame_state_.store(UiFrameState::Current, std::memory_order_release);
            }
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
        self->ProcessControls();
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
    if (!InitializeTe()) ESP_LOGW(kTag, "TE unavailable; using unsynchronized refresh");
    // Publish acceleration clients before any UI job can run on the worker.
    if (!mosaico_pxa_surface::Install(display_, [](void* context) {
            return static_cast<EspMosaicoHardware*>(context)->PresentDirectFrame();
        }, this) || esp_lv_adapter_start() != ESP_OK) return false;
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

void EspMosaicoHardware::WaitForTe(lv_area_t* area) {
    const lv_area_t dirty = *area;
    // The retained scanout is a complete image, so it is safe to widen a
    // copied dirty rectangle when the TE signal cannot admit a narrow window.
    area->x1 = 0;
    area->x2 = mosaico_board::kWidth - 1;
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
            const uint32_t blank = te_blank_us_.load();
            const auto plan = mosaico_board::PlanTransfer(dirty, measured, blank,
                                                          compact_te_enabled_.load());
            const int64_t edge = esp_timer_get_time() - age;
            const int64_t start = edge + plan.start_us;
            for (;;) {
                const int64_t remaining_us = start - esp_timer_get_time();
                if (remaining_us <= 0) break;
                vTaskDelay(std::max<TickType_t>(1,
                    pdMS_TO_TICKS((remaining_us + 999) / 1000)));
            }
            // A delayed worker must not enter an already expired compact
            // window. Try the next edge instead, retaining the same snapshot.
            if (plan.compact && esp_timer_get_time() + plan.budget_us > edge + plan.deadline_us)
                continue;
            *area = plan.area;
            scan_deadline_us_ = edge + plan.deadline_us;
            return;
        }
    }
    const uint32_t timeouts = ++te_timeouts_;
    scan_deadline_us_ = 0;
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
            (void)self->pending_region_.Take(&unused);
            self->compose_failed_ = false;
            const auto& request = self->flush_direct_request_;
            (void)self->QueuePresent(request.area, request.timestamp,
                request.frame_id, request.raster_ready_tick_us);
        }
        lv_display_flush_ready(display);
        return;
    }
    // DIRECT flush maps start at the full frame, unlike PARTIAL chunk maps.
    uint8_t* patch = pixels + area->y1 * source_stride + area->x1 * sizeof(uint16_t);
    const uint64_t timestamp = mosaico_pxa_surface::ComposeFlushArea(
        area, patch, source_stride, mosaico_pxa_surface::PixelOrder::Native);
    const bool last = lv_display_flush_is_last(display);
    // Diagnostics are painted into the owned scanout by the transfer worker.
    // Keeping them outside LVGL also covers refreshes below the top strip.
    if (!self->pending_region_.Include(*area)) {
        self->compose_failed_ = true;
        self->redraw_pending_ = true;
    }
    if (last) {
        lv_area_t dirty;
        const bool queued = self->pending_region_.Take(&dirty) && !self->compose_failed_ &&
                            self->QueuePresent(dirty, timestamp);
        self->compose_failed_ = false;
        // The worker keeps this single retained buffer until its snapshot is
        // complete. Non-last flushes need no handoff and are ready immediately.
        if (queued) return;
    }
    lv_display_flush_ready(display);
}

bool EspMosaicoHardware::TransferArea(
    const lv_area_t& dirty, const uint8_t* frame, uint32_t source_stride,
    bool last, uint64_t timestamp, const char* source_name, uint64_t frame_id) {
    if (frame == nullptr || !mosaico_board::ValidDisplayArea(dirty) ||
        source_stride < mosaico_board::kWidth * sizeof(uint16_t) ||
        source_stride % 2 != 0 || !WaitForTransfer(pdMS_TO_TICKS(10))) {
        displayed_frame_valid_ = false;
        displayed_frame_initialized_ = false;
        refresh_failed_ = true;
        return false;
    }
    lv_area_t area = dirty;
    const int64_t te_wait_started = esp_timer_get_time();
    WaitForTe(&area);
    te_wait_us_ = esp_timer_get_time() - te_wait_started;
    const uint8_t* pixels = frame + area.y1 * source_stride + area.x1 * sizeof(uint16_t);
    const int64_t transfer_started_us = esp_timer_get_time();
    last_transfer_pixels_ = lv_area_get_width(&area) * lv_area_get_height(&area);
    // Set the selected refresh window once. RAMWRC continues at the next pixel;
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
    const auto wait_transfer = [this]() {
            if (WaitForTransfer(pdMS_TO_TICKS(1000))) return true;
            ++transfer_errors_;
            ESP_LOGE(kTag, "Panel transfer timed out; retaining the DMA source");
            return false;
        };
    auto direct = mosaico_board::DirectTransferResult::Ineligible;
    if (kDirectPsramDma) {
        direct = mosaico_board::TransferContiguousRows(area, pixels, source_stride,
            wait_transfer, [this](const uint8_t* source, size_t bytes) {
                (void)xSemaphoreTake(transfer_done_, 0);
                // tx_color may have queued some chunks before a later failure.
                // Keep ownership until the IO driver has drained those chunks.
                transfer_pending_ = true;
                const esp_err_t result = esp_lcd_panel_io_tx_color(
                    panel_io_, 0x32002c00, source, bytes);
                if (result != ESP_OK) {
                    ++transfer_errors_;
                    ESP_LOGE(kTag, "Direct panel transfer failed: %s", esp_err_to_name(result));
                    if (esp_lcd_panel_io_tx_param(panel_io_, -1, nullptr, 0) == ESP_OK)
                        transfer_pending_ = false;
                    return false;
                }
                ++direct_dma_transfers_;
                return true;
            });
    }
    const bool staged = direct == mosaico_board::DirectTransferResult::Ineligible;
    if (staged) ++staged_dma_transfers_;
    const bool completed = staged ? mosaico_board::TransferBands(
        area, pixels, source_stride, transfer_pixels_, wait_transfer,
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
        [](int32_t, int32_t) {}) : direct == mosaico_board::DirectTransferResult::Completed;
    const uint32_t transfer_us = static_cast<uint32_t>(
        esp_timer_get_time() - transfer_started_us);
    transfer_duration_us_ = transfer_us;
    transfer_max_us_ = std::max(transfer_max_us_.load(), transfer_us);
    if (completed && scan_deadline_us_ != 0 &&
        esp_timer_get_time() >= scan_deadline_us_) {
        ++scan_overruns_;
        // If device scheduling exceeds the conservative quiet-window budget,
        // retain dirty snapshot copies but use full-row transfers until reboot.
        if (area.x1 != 0 || area.x2 != mosaico_board::kWidth - 1)
            compact_te_enabled_ = false;
    }
    if (!completed) {
        displayed_frame_valid_ = false;
        displayed_frame_initialized_ = false;
        refresh_failed_ = true;
    } else if (area.x1 == 0 && area.y1 == 0 &&
               area.x2 == mosaico_board::kWidth - 1 &&
               area.y2 == mosaico_board::kHeight - 1) {
        displayed_frame_initialized_ = true;
    }
    if (completed) {
        if (area.x1 != 0 || area.x2 != mosaico_board::kWidth - 1) ++compact_transfers_;
    }
    if (last) {
        if (completed && !refresh_failed_ && displayed_frame_initialized_) {
            ++completed_frames_;
            displayed_frame_valid_ = true;
            displayed_frame_id_ = frame_id != 0 ? frame_id : completed_frames_.load();
            displayed_timestamp_us_ = esp_timer_get_time();
            displayed_source_ = source_name;
            pxa_board_performance_note_frame();
            pxa_esp_surface_note_display_frame_presented(displayed_timestamp_us_);
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
    // The UI worker and direct presenter share one SRM client. Its UI operation
    // must finish before a new game conversion is submitted.
    WaitForUiSnapshot();
    if (!panel_on_.load() || (reference_ui_ != nullptr &&
        pxsys_reference_lvgl_is_locked(reference_ui_))) return false;
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
    *request = {area, timestamp, {frame_id}, raster_ready_tick_us, FlushKind::Frame};
    return true;
}

void EspMosaicoHardware::WaitForFlush() {
    // Content producers hold the display lock. Callers also take panel_mutex_
    // before reading the snapshot or changing panel controls: an idle HUD may
    // reserve the transfer slot after this wait returns.
    xSemaphoreTake(present_available_, portMAX_DELAY);
    xSemaphoreGive(present_available_);
}

void EspMosaicoHardware::WaitForUiSnapshot() {
    // The adapter uses task notifications as advisory wakeups too. Always
    // check ownership, so an unrelated/stale wake cannot release pixels early.
    // A different caller (e.g. PXADB screenshot) may own the UI lock now; the
    // bounded wait also covers a notification sent to the original producer.
    while (ui_frame_state_.load(std::memory_order_acquire) == UiFrameState::Copying)
        (void)ulTaskNotifyTake(pdTRUE, 1);
}

bool EspMosaicoHardware::QueuePresent(const lv_area_t& area, uint64_t timestamp,
                                      uint64_t frame_id, uint32_t raster_ready_tick_us) {
    // Exactly one submitted frame, plus the frame LVGL is currently drawing.
    const int64_t wait_started = esp_timer_get_time();
    xSemaphoreTake(present_available_, portMAX_DELAY);
    // A timed-out direct transfer can still read scanout. Do not exchange it
    // into the producer role, or queue a UI copy over it, until DMA is drained.
    if (!WaitForTransfer(pdMS_TO_TICKS(10))) {
        xSemaphoreGive(present_available_);
        redraw_pending_ = true;
        return false;
    }
    present_wait_us_ = esp_timer_get_time() - wait_started;
    refresh_wait_us_ += present_wait_us_.load();
    FlushRequest request = {area, timestamp, {frame_id}, raster_ready_tick_us, FlushKind::Frame};
    const auto previous = ui_frame_state_.load(std::memory_order_acquire);
    if (frame_id != 0) {
        // The composed producer remains a complete frame. Only the panel
        // window shrinks; exchanging full snapshots preserves screenshots and
        // the existing LVGL restoration/failure recovery rules.
        if (displayed_frame_valid_ && displayed_frame_initialized_) {
            const lv_area_t overlay = {
                (mosaico_board::kWidth - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2, 0,
                (mosaico_board::kWidth + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2 - 1,
                PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT - 1};
            const int64_t started = esp_timer_get_time();
            const auto damage = mosaico_board::FindFrameDamage(scanout_pixels_, present_pixels_,
                &request.area, pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY)
                    ? &overlay : nullptr);
            damage_scan_us_ = esp_timer_get_time() - started;
            if (damage == mosaico_board::FrameDamage::Unchanged) ++identical_frames_;
            if (damage != mosaico_board::FrameDamage::Unavailable &&
                (request.area.x1 || request.area.y1 ||
                 request.area.x2 != mosaico_board::kWidth - 1 ||
                 request.area.y2 != mosaico_board::kHeight - 1)) ++partial_frames_;
        }
        const int64_t snapshot_started = esp_timer_get_time();
        if (!mosaico_board::ExchangeRefreshFrame(area, present_pixels_, scanout_pixels_)) {
            xSemaphoreGive(present_available_);
            redraw_pending_ = true;
            return false;
        }
        snapshot_us_ = esp_timer_get_time() - snapshot_started;
        ui_frame_state_.store(UiFrameState::RestoreRequired, std::memory_order_release);
    } else {
        request.kind = FlushKind::UiSnapshot;
        request.ui = {lv_display_get_buf_active(display_)->data, xTaskGetCurrentTaskHandle()};
        ui_frame_state_.store(UiFrameState::Copying, std::memory_order_release);
    }
    if (xQueueSend(flush_queue_, &request, 0) == pdTRUE) return true;
    if (frame_id != 0) std::swap(present_pixels_, scanout_pixels_);
    ui_frame_state_.store(previous, std::memory_order_release);
    xSemaphoreGive(present_available_);
    redraw_pending_ = true;
    return false;
}

void EspMosaicoHardware::FlushTask(void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    lv_area_t overlay_area = {
        (mosaico_board::kWidth - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2, 0,
        (mosaico_board::kWidth + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2 - 1,
        PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT - 1};
    RoundDisplayArea(&overlay_area);
    uint32_t overlay_fps = UINT32_MAX;
    bool overlay_enabled = false;
    FlushRequest request;
    for (;;) {
        // Reuse the transfer task and retained snapshot for idle HUD updates.
        // With diagnostics disabled there is no extra periodic wakeup.
        const TickType_t timeout = pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY) &&
            self->panel_on_.load() ? pdMS_TO_TICKS(100) : portMAX_DELAY;
        const bool content = xQueueReceive(self->flush_queue_, &request, timeout) == pdTRUE;
        // A producer may already have reserved the snapshot and be about to
        // queue it. Do not race its buffer exchange or delay its content frame.
        if (!content && xSemaphoreTake(self->present_available_, 0) != pdTRUE)
            continue;
        xSemaphoreTake(self->panel_mutex_, portMAX_DELAY);
        // Idle HUD writes also need the source fence after a failed transfer.
        // Content producers already fenced this before reserving/queueing.
        if (!content && !self->WaitForTransfer(pdMS_TO_TICKS(10))) {
            self->redraw_pending_ = true;
            xSemaphoreGive(self->panel_mutex_);
            xSemaphoreGive(self->present_available_);
            continue;
        }
        const bool ui = content && request.kind == FlushKind::UiSnapshot;
        if (ui) {
            const int64_t started = esp_timer_get_time();
            const bool copied = mosaico_pxa_surface::CopyUiSnapshot(
                request.area, request.ui.pixels, self->scanout_pixels_);
            self->snapshot_us_ = esp_timer_get_time() - started;
            // Signal in this order. RENDER_START also waits for Copying to end,
            // including when flush_ready makes LVGL skip its own wait callback.
            lv_display_flush_ready(self->display_);
            self->ui_frame_state_.store(UiFrameState::Current, std::memory_order_release);
            xTaskNotifyGive(request.ui.waiter);
            if (!copied) {
                self->displayed_frame_valid_ = false;
                self->redraw_pending_ = true;
                xSemaphoreGive(self->panel_mutex_);
                xSemaphoreGive(self->present_available_);
                continue;
            }
        }
        const uint32_t fps = pxa_board_performance_refresh();
        const bool enabled = pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY);
        const bool changed = enabled != overlay_enabled || fps != overlay_fps;
        // Removing a HUD needs clean underlying pixels. The next full UI/game
        // render supplies them; never borrow a framebuffer being drawn now.
        if (!enabled && overlay_enabled) self->redraw_pending_ = true;
        if (!content && (!enabled || !changed || !self->panel_on_.load() ||
                         !self->displayed_frame_initialized_)) {
            if (!enabled) overlay_enabled = false;
            xSemaphoreGive(self->panel_mutex_);
            xSemaphoreGive(self->present_available_);
            continue;
        }
        lv_area_t dirty = content ? request.area : overlay_area;
        // A direct frame swaps the whole snapshot even when only a small panel
        // window changes. Keep its captured HUD complete outside that window.
        if (enabled && (content || changed || dirty.y1 < PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT)) {
            pxa_board_performance_draw_rgb565(
                reinterpret_cast<uint16_t*>(self->scanout_pixels_),
                mosaico_board::kWidth, mosaico_board::kHeight, mosaico_board::kWidth,
                0, 0, mosaico_board::kWidth, mosaico_board::kHeight, true);
            if (changed) {
                mosaico_board::RefreshRegion region;
                region.Include(dirty);
                region.Include(overlay_area);
                (void)region.Take(&dirty);
            }
        }
        if (content && !enabled && overlay_enabled) {
            mosaico_board::RefreshRegion region;
            region.Include(dirty);
            region.Include(overlay_area);
            (void)region.Take(&dirty);
        }
        const uint64_t frame_id = content && !ui ? request.frame_id : 0;
        const bool completed = self->TransferArea(dirty, self->scanout_pixels_,
            mosaico_board::kWidth * sizeof(uint16_t), content,
            content ? request.timestamp : 0,
            frame_id ? "surface-direct" : "lvgl-composited", frame_id);
        if (!content) {
            // A failed diagnostic stripe must request a clean recovery frame
            // without carrying its failure into the next content refresh.
            self->refresh_failed_ = false;
            if (!completed) self->redraw_pending_ = true;
        }
        if (completed) {
            overlay_fps = fps;
            overlay_enabled = enabled;
        }
        // HUD-only transfers do not complete a new content frame. Neither the
        // on-screen FPS nor PXADB PERF should count their own instrumentation.
        if (content && completed && frame_id) {
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
    // Logical pointers also serve PXADB when physical touch is unavailable.
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
    ESP_LOGI(kTag, "CST92xx two-contact polling ready on GPIO%d", mosaico_board::kTouchInterrupt);
    return true;
}

void EspMosaicoHardware::PollTouch(void* context) {
    auto* self = static_cast<EspMosaicoHardware*>(context);
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        if (self->power_action_.load() >= 0) self->PerformPowerAction();
        esp_lcd_touch_point_data_t raw[EspMosaicoTouchState::kMaxPoints] = {};
        uint8_t count = 0;
        esp_err_t status = self->touch_ != nullptr
            ? esp_lcd_touch_read_data(self->touch_) : ESP_ERR_INVALID_STATE;
        if (status == ESP_OK)
            status = esp_lcd_touch_get_data(
                self->touch_, raw, &count, EspMosaicoTouchState::kMaxPoints);
        if (self->touch_ != nullptr) self->touch_reads_.fetch_add(1);
        self->touch_last_status_.store(status);
        if (self->touch_ != nullptr && status != ESP_OK) self->touch_errors_.fetch_add(1);
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
        const bool takeover = self->input_.UpdatePhysical(points, active,
            timestamp_us, status == ESP_OK);
        const auto callback = takeover ? self->input_takeover_ : nullptr;
        void* callback_context = self->input_takeover_context_;
        const bool pressed = status == ESP_OK && active > 0;
        portEXIT_CRITICAL(&self->touch_lock_);
        // PXADB may cancel injected pointers here. It must run outside the lock.
        if (callback != nullptr) callback(callback_context);
        if (pressed) self->touch_reports_.fetch_add(1);
        self->PollBattery(static_cast<uint32_t>(timestamp_us / 1000));
        // A bus timeout or cancelled shutdown can miss many poll intervals.
        // Rebase instead of replaying them in a priority-6 catch-up burst.
        const TickType_t now = xTaskGetTickCount();
        if (now - next > pdMS_TO_TICKS(10)) next = now;
        xTaskDelayUntil(&next, pdMS_TO_TICKS(10));
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
        portENTER_CRITICAL(&self->touch_lock_);
        const size_t count = self->input_.Points(points);
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
    const bool queued = self->input_.pending(context->slot);
    const auto sample = self->input_.Read(context->slot);
    data->continue_reading = self->input_.pending(context->slot);
    portEXIT_CRITICAL(&self->touch_lock_);
    if (sample.cancelled) lv_indev_wait_release(input);
    data->state = sample.event.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = sample.event.point.x;
    data->point.y = sample.event.point.y;
    data->timestamp = MosaicoTouchLvglTimestamp(sample.timestamp_us,
        esp_timer_get_time(), lv_tick_get(), queued);
#if CONFIG_LV_USE_GESTURE_RECOGNITION
    if (context->slot == 0) MosaicoSetGestureMetadata(input, data);
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
    if (iot_button_register_cb(button_, BUTTON_DOUBLE_CLICK, nullptr,
        [](void*, void* context) {
            static_cast<EspMosaicoHardware*>(context)->power_menu_pending_.store(true);
        }, this) != ESP_OK)
        return false;
    return iot_button_register_cb(button_, BUTTON_LONG_PRESS_START, nullptr,
        [](void*, void* context) {
            static_cast<EspMosaicoHardware*>(context)->provisioning_pending_.store(true);
        }, this) == ESP_OK;
}

bool EspMosaicoHardware::InitializeBootButton() {
    const button_config_t config = {};
    const button_gpio_config_t gpio_config = {
        .gpio_num = mosaico_board::kBootButton, .active_level = 0,
    };
    if (iot_button_new_gpio_device(&config, &gpio_config, &boot_button_) != ESP_OK) return false;
    return iot_button_register_cb(boot_button_, BUTTON_SINGLE_CLICK, nullptr,
        [](void*, void* context) {
            static_cast<EspMosaicoHardware*>(context)->boot_pending_ = true;
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
    boot_screen_.Hide();
    lv_display_trigger_activity(display_);
    if (reference_ui_ != nullptr) {
        (void)pxsys_reference_lvgl_set_lock_changed_callback(
            reference_ui_, this, [](void* context, bool locked) {
                auto* self = static_cast<EspMosaicoHardware*>(context);
                if (locked) (void)EspMosaicoVibrate(0);
                self->UpdateInteractivity();
                if (!locked) {
                    // Defer redraw until the lock callback and input event
                    // have finished, so composition sees the unlocked state.
                    self->wake_pending_ = true;
                }
            });
        (void)pxsys_reference_lvgl_set_power_action_callback(
            reference_ui_, this, [](void* context, pxsys_reference_power_action_t action) {
                (void)static_cast<EspMosaicoHardware*>(context)->RequestPowerAction(action);
            });
        (void)pxsys_reference_lvgl_set_power_menu_changed_callback(
            reference_ui_, nullptr, [](void*, bool visible) {
                pxa_esp_surface_set_power_overlay_visible(visible);
            });
    }
    // The initial lock screen predates the callback. Publish it explicitly.
    UpdateInteractivity();
    (void)lv_timer_create([](lv_timer_t* timer) {
        auto* self = static_cast<EspMosaicoHardware*>(lv_timer_get_user_data(timer));
        self->PublishStatus();
        self->PublishDiagnostics();
    }, 100, this);
    PublishStatus();
}

void EspMosaicoHardware::ProcessControls() {
    // Reuse the existing 16-ms display timer. Status/network polling remains
    // on its 100-ms timer; Home, Back and wake no longer wait for that timer.
    if (system_ == nullptr || power_action_.load() >= 0) return;
    if (boot_pending_.exchange(false)) {
        if (!panel_on_.load() || (reference_ui_ && pxsys_reference_lvgl_is_locked(reference_ui_)))
            wake_pending_ = true;
        else back_pending_ = true;
    }
    if (home_pending_.exchange(false)) {
        lv_display_trigger_activity(display_);
        if (!panel_on_ || (reference_ui_ != nullptr &&
            pxsys_reference_lvgl_is_locked(reference_ui_))) {
            wake_pending_ = true;
        } else if (reference_ui_ != nullptr) {
            (void)pxsys_reference_lvgl_home(reference_ui_);
        }
    }
    if (provisioning_pending_.exchange(false) && wifi_initialized_) {
        wifi_enabled_ = true;
        WifiManager::GetInstance().StartConfigAp();
    }
    if (power_menu_pending_.exchange(false) && reference_ui_ != nullptr) {
        if (!panel_on_.load()) wake_pending_ = true;
        (void)pxsys_reference_lvgl_show_power_menu(reference_ui_);
    }
    if (back_pending_.exchange(false)) {
        const bool dismissed = reference_ui_ != nullptr &&
            pxsys_reference_lvgl_dismiss_overlay(reference_ui_);
        if (!dismissed) {
            pxsys_back_result_t result;
            (void)pxsys_task_manager_back(pxsys_standard_system_tasks(system_), &result);
        }
    }
    if (wake_pending_.exchange(false)) WakePanel();
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

void EspMosaicoHardware::UpdateInteractivity() {
    const bool interactive = panel_on_.load() && power_action_.load() < 0 &&
        (reference_ui_ == nullptr || !pxsys_reference_lvgl_is_locked(reference_ui_));
    pxa_esp_surface_set_display_unlocked(interactive);
    (void)pxa_host_set_display_interactive(interactive);
}

bool EspMosaicoHardware::RequestPowerAction(pxsys_reference_power_action_t action) {
    if (action != PXSYS_REFERENCE_POWER_ACTION_RESTART &&
        action != PXSYS_REFERENCE_POWER_ACTION_SHUTDOWN) return false;
    if (touch_task_ == nullptr) {
        ESP_LOGE(kTag, "System power action requires the input worker");
        return false;
    }
    int8_t expected = -1;
    if (!power_action_.compare_exchange_strong(expected, static_cast<int8_t>(action)))
        return expected == static_cast<int8_t>(action);
    // The existing input worker executes this within its next 10-ms poll.
    // No extra task/stack allocation and no UI-thread save/detach deadlock.
    return true;
}

void EspMosaicoHardware::PerformPowerAction() {
    pxa_esp_surface_set_display_unlocked(false);
    (void)pxa_host_set_display_interactive(false);
    const bool saved = pxa_host_prepare_shutdown(1200);
    if (!saved) {
        // A queued stop can still complete later. Do not cut power while its
        // save/detach work is outstanding; resume the UI and allow a retry.
        ESP_LOGE(kTag, "Power action cancelled: Guest shutdown was not acknowledged");
        // Clear the gate before notifying the other core's UI timer; its wake
        // callback must see that interaction can resume.
        power_action_ = -1;
        power_menu_pending_ = true;
        wake_pending_ = true;
        return;
    }
    (void)pxa_system_clock_save();
    if (!EspMosaicoStopPeripherals()) {
        ESP_LOGE(kTag, "Power action cancelled: expansion resource cleanup failed");
        power_action_ = -1; power_menu_pending_ = true; wake_pending_ = true;
        return;
    }
    audio_.StopForShutdown();
    (void)EspMosaicoVibrate(0);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (power_action_.load() == PXSYS_REFERENCE_POWER_ACTION_RESTART) {
        esp_restart();
    }
    if (esp_lv_adapter_lock(2000) == ESP_OK) {
        lv_lock();
        IdleScreenOff();
        lv_unlock();
        esp_lv_adapter_unlock();
    }
    // Match the board's SAM8108 shutdown latch: GPIO57 is open drain, active
    // low. Keep the saved volume/brightness and all storage partitions intact.
    (void)gpio_set_level(static_cast<gpio_num_t>(mosaico_board::kAudioAmplifierEnable), 0);
    ESP_LOGI(kTag, "Requesting SAM8108 power-off on GPIO%d", mosaico_board::kPowerSwitch);
    for (;;) {
        (void)ConfigureOutput(mosaico_board::kPowerSwitch, 0, GPIO_MODE_OUTPUT_OD);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void EspMosaicoHardware::PollBattery(uint32_t now_ms) {
    battery_.Poll(now_ms, [this](uint8_t reg, uint8_t* bytes, size_t size) {
        if (battery_device_ == nullptr) {
            if (i2c_master_probe(i2c_bus_, 0x55, 2) != ESP_OK) return false;
            esp_rom_delay_us(80);
            i2c_device_config_t config = {};
            config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            config.device_address = 0x55;
            config.scl_speed_hz = 400000;
            if (i2c_master_bus_add_device(i2c_bus_, &config, &battery_device_) != ESP_OK)
                return false;
        }
        const esp_err_t result = i2c_master_transmit_receive(
            battery_device_, &reg, 1, bytes, size, 2);
        // TI SLUUBD4A section 5.3 requires at least 66 us between packets
        // addressed to the gauge at 400 kHz. No other task accesses this device.
        esp_rom_delay_us(80);
        return result == ESP_OK;
    });
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
    pxa_esp_surface_set_display_unlocked(false);
    (void)pxa_host_set_display_interactive(false);
    WaitForFlush();
    xSemaphoreTake(panel_mutex_, portMAX_DELAY);
    if (panel_on_ && esp_lcd_panel_disp_on_off(panel_, false) == ESP_OK)
        panel_on_ = false;
    xSemaphoreGive(panel_mutex_);
    UpdateInteractivity();
}

void EspMosaicoHardware::ShowBootFrame() {
    if (esp_lv_adapter_lock(2000) != ESP_OK) return;
    lv_lock();
    boot_screen_.Show(display_);
    ShowInitialFrame();
    ESP_LOGI(kTag, "Boot frame visible at %lld ms", esp_timer_get_time() / 1000);
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
    UpdateInteractivity();
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
    (void)pxa_system_clock_update_status(&status);
    const auto battery = battery_.Get();
    status.battery_valid = battery.valid;
    status.battery_percent = battery.percent;
    status.charging = battery.charging;
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
             touch_ != nullptr && touch_task_ != nullptr,
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
    ESP_LOGI(kTag, "frame_stats ui_period_ms=%u pipeline=async fps=%u.%u "
             "render_us=%lu render_max_us=%lu present_wait_us=%lu te_wait_us=%lu "
             "tx_us=%lu tx_max_us=%lu scan_overruns=%lu time_valid=%d sram=%u",
             static_cast<unsigned>(mosaico_board::kUiRefreshPeriodMs),
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
    ESP_LOGI(kTag, "ui_transfer compact_total=%lu compact_enabled=%d draw_rows=%u",
             static_cast<unsigned long>(compact_transfers_.load()),
             compact_te_enabled_.load(),
             static_cast<unsigned>(mosaico_board::kDrawBufferRows));
    ESP_LOGI(kTag, "transfer_path psram_direct=%lu staged=%lu enabled=%d",
             static_cast<unsigned long>(direct_dma_transfers_.load()),
             static_cast<unsigned long>(staged_dma_transfers_.load()), kDirectPsramDma);
    ESP_LOGI(kTag, "damage_stats compare_us=%lu partial_frames=%lu identical_frames=%lu tx_pixels=%lu",
             static_cast<unsigned long>(damage_scan_us_.load()),
             static_cast<unsigned long>(partial_frames_.load()),
             static_cast<unsigned long>(identical_frames_.load()),
             static_cast<unsigned long>(last_transfer_pixels_.load()));
    ESP_LOGI(kTag, "power idle_ms=%lu locked=%d panel_on=%d dim=%d touch_clock=lvgl",
             static_cast<unsigned long>(lv_display_get_inactive_time(display_)),
             reference_ui_ != nullptr && pxsys_reference_lvgl_is_locked(reference_ui_),
             panel_on_.load(), idle_dim_enabled_.load());
    ESP_LOGI(kTag, "surface_stats presenter_core=1 raster_us=%lu compose_us=%lu snapshot_us=%lu ppa_frames=%lu cpu_frames=%lu alpha_frames=%lu ppa_background=%lu flush_stack_free=%lu",
             static_cast<unsigned long>(direct_raster_us_.load()),
             static_cast<unsigned long>(direct_compose_us_.load()),
             static_cast<unsigned long>(snapshot_us_.load()),
             static_cast<unsigned long>(direct_ppa_frames_.load()),
             static_cast<unsigned long>(direct_cpu_frames_.load()),
             static_cast<unsigned long>(direct_alpha_frames_.load()),
             static_cast<unsigned long>(mosaico_pxa_surface::AcceleratedFillCount()),
             static_cast<unsigned long>(uxTaskGetStackHighWaterMark(flush_task_)));
}
