// SPDX-License-Identifier: Apache-2.0
#include "pxa_board.h"
#include "doit_s31_panel.h"
#include "doit_s31_audio.h"
#include "doit_s31_surface.h"
#include "doit_s31_scanout.h"
#include "doit_s31_touch.h"
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/sdmmc_host.h>
#include <driver/uart.h>
#include <driver/uart_vfs.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_lcd_panel_rgb.h>
#include "doit_s31_config.h"

#include <esp_log.h>
#include <esp_lv_decoder.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <pxa_board_common/boot_screen.h>
#include <pxa_board_common/ledc_backlight.h>
#include <pxa_board_common/sd_volume.h>
#include <pxadb/pxadb_service.h>
#include <pxadb_pointer_samples.h>
#include <wifi_manager.h>
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#endif

namespace {
constexpr int kWidth = 480, kHeight = 854;
constexpr size_t kPixels = size_t(kWidth) * kHeight, kBytes = kPixels * 2;
constexpr char kTag[] = "doit_s31";
class Board {
public:
    bool Initialize();
    bool Lock(uint32_t ms) {
        return mutex_ && xSemaphoreTakeRecursive(mutex_, ms ? pdMS_TO_TICKS(ms) : portMAX_DELAY) == pdTRUE;
    }
    void Unlock() { xSemaphoreGiveRecursive(mutex_); }
    lv_display_t* display() { return display_; }
    void Attach(pxsys_standard_system_t*, pxsys_reference_lvgl_t*);
    void InitialFrame();
    bool SetWifi(bool);
    bool SetLevel(pxsys_level_control_t, uint8_t);
    void Dim(bool on, uint8_t value) { light_.SetIdleDim(on, value); ApplyBacklight(); }
    void ScreenOff() {
        screen_on_ = false; ApplyBacklight();
        pxa_esp_surface_set_display_unlocked(false);
        (void)pxa_host_set_display_interactive(false);
    }
    bool Capture(uint16_t*, size_t, bool, pxadb::TestControlCaptureInfo* = nullptr);
    bool Diagnostics();
private:
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
    void PanelDiagnosticKey(pxadb::TestControlKey);
    void PanelDiagnosticDraw(uint16_t*);
    unsigned diagnostic_clock_ = 0, diagnostic_pattern_ = 0, diagnostic_light_ = 0;
    bool diagnostic_phase_ = false;
#endif
    bool Touch();
    bool Pointers();
    void CancelPointers();
    void Sd();
    void Loop();
    void Input();
    bool Wake();
    void Key(pxadb::TestControlKey);
    void Status();
    void CollectCompletions();
    void WaitScan();
    void ApplyBacklight();
    static bool IRAM_ATTR Vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*);
    static bool IRAM_ATTR FrameComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*);
    static void Flush(lv_display_t*, const lv_area_t*, uint8_t*);
    SemaphoreHandle_t mutex_ = nullptr, frame_done_ = nullptr, scan_ready_ = nullptr;
    StaticSemaphore_t frame_done_storage_{}, scan_ready_storage_{};
    esp_lcd_panel_handle_t panel_ = nullptr;
    i2c_master_bus_handle_t bus_ = nullptr;
    bool touch_ = false;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    uint8_t touch_slots_ = 0;
    uint32_t touch_success_ms_ = 0;
    doit_s31::TouchReport physical_{};
    esp_lv_decoder_handle_t decoder_ = nullptr;
    uint16_t* frames_[2]{};
    portMUX_TYPE scan_lock_ = portMUX_INITIALIZER_UNLOCKED;
    doit_s31::ScanoutClock scan_clock_;
    struct Presentation {
        uint64_t frame_id, input_us;
        uint32_t raster_us, fence;
        unsigned buffer;
        bool capture;
    };
    std::array<Presentation, 4> pending_{};
    unsigned pending_head_ = 0, pending_count_ = 0;
    unsigned front_ = 0;
    uint64_t completed_ = 0, completed_us_ = 0;
    bool frame_valid_ = false, capture_refresh_ = false;
    std::atomic<bool> backlight_ready_{false};
    uint64_t boot_visible_us_ = 0;
    unsigned missed_surfaces_ = 0;
    std::atomic<uint32_t> light_state_{UINT32_MAX};
    lv_display_t* display_ = nullptr;
    lv_indev_t* pointers_[doit_s31::kTouchPointers]{};
    bool pressed_ = false, touch_blocked_ = false;
    std::atomic<bool> surface_dirty_{true}, wifi_ready_{false};
    bool screen_on_ = true, audio_ready_ = false;
    std::atomic<bool> wifi_enabled_{true};
    pxa_board_common::LedcBacklight light_;
    pxa_board_common::BootScreen boot_;
    DoitS31Audio audio_;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* ui_ = nullptr;
    pxadb::PointerSamples<doit_s31::kTouchPointers> samples_;
    pxadb::TestControlTakeoverCallback takeover_ = nullptr;
    void* takeover_context_ = nullptr;
};

bool Board::Initialize() {
    // Route console and PXADB through one UART driver. Polled ROM/stdio TX
    // alongside buffered protocol TX can splice logs into screenshot frames.
    const uart_config_t console = {.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    if (uart_param_config(UART_NUM_0, &console) != ESP_OK ||
        uart_set_pin(UART_NUM_0, 58, 59, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(UART_NUM_0, 4096, 0, 0, nullptr, 0) != ESP_OK) return false;
    uart_vfs_dev_use_driver(UART_NUM_0);
    mutex_ = xSemaphoreCreateRecursiveMutex();
    // The DMA ISR must access this queue while Flash/PSRAM caches are disabled.
    frame_done_ = xSemaphoreCreateBinaryStatic(&frame_done_storage_);
    scan_ready_ = xSemaphoreCreateBinaryStatic(&scan_ready_storage_);
    if (!mutex_ || !frame_done_ || !scan_ready_) return false;
    const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK};
    const ledc_channel_config_t channel = {.gpio_num = doit_s31::kBacklight, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0};
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&channel) != ESP_OK) return false;
    light_.Configure(LEDC_CHANNEL_0);
    if (doit_s31_panel_create(&panel_) != ESP_OK ||
        esp_lcd_rgb_panel_get_frame_buffer(panel_, 2,
            reinterpret_cast<void**>(&frames_[0]), reinterpret_cast<void**>(&frames_[1])) != ESP_OK) return false;
    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_vsync = Vsync, .on_frame_buf_complete = FrameComplete,
    };
    if (esp_lcd_rgb_panel_register_event_callbacks(panel_, &callbacks, this) != ESP_OK ||
        esp_lcd_panel_init(panel_) != ESP_OK) return false;
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return esp_timer_get_time() / 1000; });
    display_ = lv_display_create(kWidth, kHeight);
    if (!display_) return false;
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(display_, this);
    // LVGL redraws the complete trusted UI before guest composition. Render
    // directly into the two DMA buffers, starting away from scanout buffer 0.
    // FULL mode is required: a retained partial background would contain guests.
    lv_display_set_buffers(display_, frames_[1], frames_[0], kBytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display_, Flush);
    if (esp_lv_decoder_init(&decoder_) != ESP_OK) return false;
    boot_.Show(display_);
    InitialFrame();
    if (!Touch()) ESP_LOGW(kTag, "Touch unavailable; PXADB input remains available");
    if (!Pointers()) return false;
    audio_ready_ = audio_.Initialize(bus_);
    if (!audio_ready_) ESP_LOGW(kTag, "Audio unavailable");
    Sd();
    pxa_esp_surface_set_frame_ready_callback([](void* c) {
        static_cast<Board*>(c)->surface_dirty_.store(true);
    }, this);
    if (xTaskCreatePinnedToCore([](void* c) { static_cast<Board*>(c)->Loop(); },
        "doit_s31_ui", 12288, this, 4, nullptr, 0) != pdPASS) return false;
    if (xTaskCreate([](void* c) {
        auto* b = static_cast<Board*>(c);
        WifiManagerConfig config; config.ssid_prefix = "PXA-DOIT-S31"; config.language = "zh-CN";
        b->wifi_enabled_ = pxa_board_wifi_get_enabled();
        if (WifiManager::GetInstance().Initialize(config)) {
            esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            clock.wait_for_sync = false;
            (void)esp_netif_sntp_init(&clock);
            if (b->wifi_enabled_) WifiManager::GetInstance().StartStation();
            b->wifi_ready_ = true;
        }
        vTaskDelete(nullptr);
    }, "doit_s31_wifi", 6144, this, 2, nullptr) != pdPASS) return false;
    ESP_LOGI(kTag, "ST7102 480x854 RGB565 36MHz / BOOT/LCD-CS GPIO61 (download only) / UART0 921600");
    return true;
}

void Board::Flush(lv_display_t* display, const lv_area_t*, uint8_t* data) {
    auto* b = static_cast<Board*>(lv_display_get_user_data(display));
    b->CollectCompletions();
    auto* output = reinterpret_cast<uint16_t*>(data);
    const unsigned next = output == b->frames_[0] ? 0 : 1;
    if (output != b->frames_[next] || b->pending_count_ == b->pending_.size()) abort();
    pxa_esp_surface_frame_t frame{};
    const bool acquired = pxa_esp_surface_acquire_latest(&frame);
    if (!acquired && pxa_esp_surface_has_visible_surface()) ++b->missed_surfaces_;
    if (acquired) doit_s31::Compose(frame, output, 0, 0, kWidth, kHeight, kWidth);
    if (pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY))
        pxa_board_performance_draw_rgb565(output, kWidth, kHeight, kWidth,
            0, 0, kWidth, kHeight, false);
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
    b->PanelDiagnosticDraw(output);
#endif
    const uint64_t frame_id = acquired ? frame.frame_id : 0;
    const uint64_t input_us = acquired ? frame.input_timestamp_us : 0;
    const uint32_t raster_us = acquired ? frame.raster_ready_tick_us : 0;
    if (acquired) pxa_esp_surface_release_frame(frame.lease);
    (void)xSemaphoreTake(b->frame_done_, 0);
    const auto result = esp_lcd_panel_draw_bitmap(b->panel_, 0, 0, kWidth, kHeight, output);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "RGB presentation failed: %s", esp_err_to_name(result));
        abort(); // Buffer ownership is unknown after a failed presentation.
    }
    // S31 RGB emits a link-switch completion only after a submission. It is
    // distinct from the periodic VSYNC used for scan timestamps and fences.
    if (xSemaphoreTake(b->frame_done_, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(kTag, "RGB buffer handoff stalled"); abort();
    }
    portENTER_CRITICAL(&b->scan_lock_);
    const uint32_t fence = b->scan_clock_.Fence();
    portEXIT_CRITICAL(&b->scan_lock_);
    b->pending_[(b->pending_head_ + b->pending_count_++) % b->pending_.size()] =
        {frame_id, input_us, raster_us, fence, next, b->capture_refresh_};
    b->CollectCompletions();
    b->ApplyBacklight();
    lv_display_flush_ready(display);
}

bool IRAM_ATTR Board::Vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void* c) {
    auto* b = static_cast<Board*>(c);
    portENTER_CRITICAL_ISR(&b->scan_lock_);
    b->scan_clock_.Complete(uint32_t(esp_timer_get_time()));
    portEXIT_CRITICAL_ISR(&b->scan_lock_);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(b->scan_ready_, &woken);
    return woken == pdTRUE;
}
bool IRAM_ATTR Board::FrameComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void* c) {
    auto* b = static_cast<Board*>(c);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(b->frame_done_, &woken);
    return woken == pdTRUE;
}

void Board::ApplyBacklight() {
    const bool on = screen_on_ && backlight_ready_.load();
    const uint32_t state = (uint32_t(on) << 16) | (uint32_t(light_.percent()) << 8) |
                           light_.idle_dim_percent();
    if (light_state_.exchange(state) != state)
        ESP_LOGI(kTag, "Backlight: enabled=%u brightness=%u idle_limit=%u", unsigned(on),
                 unsigned(light_.percent()), unsigned(light_.idle_dim_percent()));
    light_.Apply(on);
}

void Board::WaitScan() {
    if (xSemaphoreTake(scan_ready_, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(kTag, "RGB scanout stalled");
        abort();
    }
}

void Board::CollectCompletions() {
    while (pending_count_) {
        const auto& p = pending_[pending_head_];
        uint64_t timestamp;
        portENTER_CRITICAL(&scan_lock_);
        const bool ready = scan_clock_.Timestamp(p.fence, esp_timer_get_time(), timestamp);
        portEXIT_CRITICAL(&scan_lock_);
        if (!ready) break;
        front_ = p.buffer; frame_valid_ = true;
        completed_us_ = timestamp; ++completed_;
        if (!p.capture) pxa_board_performance_note_frame();
        pxa_esp_surface_note_display_frame_presented(timestamp);
        if (p.frame_id) {
            pxa_esp_surface_note_game_frame_presented(p.frame_id, timestamp);
            pxa_esp_surface_note_display_latency(p.frame_id, p.input_us, p.raster_us, timestamp);
        }
        pending_head_ = (pending_head_ + 1) % pending_.size();
        --pending_count_;
    }
}

void Board::InitialFrame() {
    if (!Lock(1000)) return;
    lv_lock();
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    lv_refr_now(display_);
    CollectCompletions();
    while (pending_count_) { WaitScan(); CollectCompletions(); }
    if (!backlight_ready_.load() && frame_valid_) {
        // Let the TDDI settle with a fully scanned logo, keeping PWM at zero.
        vTaskDelay(pdMS_TO_TICKS(100));
        boot_visible_us_ = esp_timer_get_time();
        backlight_ready_ = true;
        ApplyBacklight();
        ESP_LOGI(kTag, "Boot logo visible at %lu ms, completed scan=%llu us",
            static_cast<unsigned long>(esp_log_timestamp()), completed_us_);
    }
    lv_unlock(); Unlock();
}

bool Board::Touch() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0; bus_config.sda_io_num = GPIO_NUM_0;
    bus_config.scl_io_num = GPIO_NUM_1; bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7; bus_config.flags.enable_internal_pullup = true;
    // SPI initialization is finished and CS remains high before this bus owns GPIO0/1.
    if (i2c_new_master_bus(&bus_config, &bus_) != ESP_OK) return false;
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = 0x55; io_config.control_phase_bytes = 1;
    io_config.lcd_cmd_bits = 16; io_config.scl_speed_hz = 100000;
    io_config.transaction_timeout_ms = 20;
    io_config.flags.disable_control_phase = true;
    if (esp_lcd_new_panel_io_i2c(bus_, &io_config, &touch_io_) != ESP_OK) return false;
    if (i2c_master_probe(bus_, 0x55, 20) != ESP_OK) {
        ESP_LOGW(kTag, "ST7102 touch at 0x55 did not ACK; check the separate JP4 cable");
        for (unsigned address : {0x14u, 0x18u, 0x38u, 0x5du})
            if (i2c_master_probe(bus_, address, 20) == ESP_OK)
                ESP_LOGI(kTag, "Shared I2C0 ACK: 0x%02x", address);
        return false;
    }
    uint8_t info[16]{};
    if (esp_lcd_panel_io_rx_param(touch_io_, 0, info, sizeof(info)) != ESP_OK ||
        !info[9] || info[9] > doit_s31::kTouchPointers) return false;
    touch_slots_ = info[9];
    const unsigned width = (info[5] & 0x3f) * 256u + info[6];
    const unsigned height = (info[7] & 0x3f) * 256u + info[8];
    ESP_LOGI(kTag, "ST7102 touch FW %02x, revision %02x%02x%02x%02x, range %ux%u",
        info[0], info[12], info[13], info[14], info[15], width, height);
    if (width != kWidth || height != kHeight) return false;
    touch_ = true;
    touch_success_ms_ = lv_tick_get();
    ESP_LOGI(kTag, "ST7102 touch ready at I2C0 0x55: %u independent fingers", touch_slots_);
    return true;
}

bool Board::Pointers() {
    for (unsigned id = 0; id < doit_s31::kTouchPointers; ++id) {
        auto* pointer = lv_indev_create();
        if (!pointer) return false;
        pointers_[id] = pointer;
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(pointer, display_);
        lv_indev_set_driver_data(pointer, this);
        lv_indev_set_user_data(pointer, reinterpret_cast<void*>(uintptr_t(id)));
        lv_timer_set_period(lv_indev_get_read_timer(pointer), 5);
        lv_indev_set_read_cb(pointer, [](lv_indev_t* input, lv_indev_data_t* data) {
            auto* b = static_cast<Board*>(lv_indev_get_driver_data(input));
            const auto id = uint8_t(reinterpret_cast<uintptr_t>(lv_indev_get_user_data(input)));
            bool more = false;
            const auto sample = b->samples_.Read(id, more);
            if (sample.cancel) lv_indev_wait_release(input);
            data->point = {sample.x, sample.y};
            data->timestamp = lv_tick_get();
            data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->continue_reading = more;
        });
    }
    return true;
}

void Board::CancelPointers() {
    for (unsigned id = 0; id < doit_s31::kTouchPointers; ++id) (void)samples_.Cancel(id);
}

void Board::Sd() {
    const gpio_config_t power = {.pin_bit_mask = 1ULL << doit_s31::kSdPower, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&power) != ESP_OK || gpio_set_level(gpio_num_t(doit_s31::kSdPower), 0) != ESP_OK) return;
    vTaskDelay(pdMS_TO_TICKS(50));
    // Card detect is unconnected. Probe once, preserve contents on mount failure.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0; host.max_freq_khz = 20000;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4; slot.clk = GPIO_NUM_24; slot.cmd = GPIO_NUM_25;
    slot.d0 = GPIO_NUM_20; slot.d1 = GPIO_NUM_21; slot.d2 = GPIO_NUM_22; slot.d3 = GPIO_NUM_23;
    slot.cd = GPIO_NUM_NC;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {.format_if_mount_failed = false,
        .max_files = 8, .allocation_unit_size = 16384};
    sdmmc_card_t* card = nullptr;
    const auto result = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
    if (result == ESP_OK) {
        const auto registered = pxa_board::RegisterSdVolume("/sdcard", card);
        ESP_LOGI(kTag, "SDMMC0 volume: %ld", long(registered));
    } else ESP_LOGW(kTag, "SD card unavailable (%s); contents preserved", esp_err_to_name(result));
}

bool Board::Wake() {
    const bool woke = !screen_on_;
    screen_on_ = true; ApplyBacklight();
    lv_display_trigger_activity(display_);
    return woke;
}
void Board::Input() {
    const uint32_t now = lv_tick_get();
    if (touch_ && touch_slots_) {
        doit_s31::TouchReport report;
        const bool ok = doit_s31::ReadTouch([this](int reg, uint8_t* p, size_t n) {
            return esp_lcd_panel_io_rx_param(touch_io_, reg, p, n) == ESP_OK;
        }, touch_slots_, kWidth, kHeight, report);
        if (ok) {
            touch_success_ms_ = now;
            const bool any = std::any_of(report.begin(), report.end(),
                [](const auto& p) { return p.pressed; });
            const auto count = std::count_if(report.begin(), report.end(),
                [](const auto& p) { return p.pressed; });
            const auto previous_count = std::count_if(physical_.begin(), physical_.end(),
                [](const auto& p) { return p.pressed; });
            if (count != previous_count && (count >= 2 || previous_count >= 2))
                ESP_LOGI(kTag, "Physical touch contacts: %u", unsigned(count));
            if (any && !pressed_) {
                CancelPointers();
                if (takeover_) takeover_(takeover_context_);
                touch_blocked_ = Wake() || touch_blocked_;
            }
            if (any) Wake();
            else touch_blocked_ = false;
            if (!touch_blocked_) for (unsigned id = 0; id < report.size(); ++id) {
                const auto& p = report[id];
                const auto& previous = physical_[id];
                if ((p.pressed || previous.pressed) &&
                    !samples_.Push(id, p.pressed ? p.x : previous.x,
                        p.pressed ? p.y : previous.y, p.pressed)) {
                    CancelPointers(); touch_blocked_ = any; break;
                }
            }
            physical_ = report; pressed_ = any;
        } else if (pressed_ && now - touch_success_ms_ >= 150) {
            CancelPointers(); physical_ = {}; pressed_ = false; touch_blocked_ = true;
            ESP_LOGW(kTag, "Touch read timed out; all fingers cancelled until release");
        }
    }

}
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
void Board::PanelDiagnosticDraw(uint16_t* output) {
    if (!diagnostic_pattern_) return;
    const uint16_t background = diagnostic_pattern_ == 2 ? 0x7bef : 0;
    std::fill_n(output, kPixels, background);
    if (diagnostic_pattern_ != 3) return;
    for (int y = 150; y < 650; ++y) for (int x = 40; x < 440; ++x) {
        uint16_t color = 0;
        // Sparse high-contrast bars leave large black regions for ghost checks.
        if ((y >= 180 && y < 188) || (y >= 350 && y < 366)) color = 0xffff;
        else if (y >= 520 && y < 580)
            color = x < 170 ? 0xf800 : x < 310 ? 0x07e0 : 0x001f;
        output[y*kWidth+x] = color;
    }
}
void Board::PanelDiagnosticKey(pxadb::TestControlKey key) {
    constexpr uint32_t clocks[] = {36000000, 30000000, 24000000, 20000000};
    constexpr uint8_t lights[] = {75, 25, 5};
    switch (key) {
        case pxadb::TestControlKey::kVolumeUp:
            WaitScan();
            diagnostic_phase_ = !diagnostic_phase_;
            esp_rom_gpio_connect_out_signal(43, LCD_PCLK_PAD_OUT_IDX, diagnostic_phase_, false);
            break;
        case pxadb::TestControlKey::kVolumeDown: {
            const unsigned next = (diagnostic_clock_+1) % 4;
            if (esp_lcd_rgb_panel_set_pclk(panel_, clocks[next]) == ESP_OK) diagnostic_clock_ = next;
            break;
        }
        case pxadb::TestControlKey::kHome:
            diagnostic_pattern_ = (diagnostic_pattern_+1) % 4;
            break;
        case pxadb::TestControlKey::kBack:
            diagnostic_light_ = (diagnostic_light_+1) % 3;
            light_.SetPercent(lights[diagnostic_light_]); ApplyBacklight();
            break;
    }
    surface_dirty_ = true;
    Status();
    ESP_LOGW(kTag, "PANEL COMPARE: pclk=%lu phase_inverted=%u pattern=%u brightness=%u",
        (unsigned long)clocks[diagnostic_clock_], unsigned(diagnostic_phase_),
        diagnostic_pattern_, unsigned(light_.percent()));
}
#endif
void Board::Key(pxadb::TestControlKey key) {
    if (Wake() || !ui_) return;
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
    PanelDiagnosticKey(key);
    return;
#endif
    if (key == pxadb::TestControlKey::kHome) (void)pxsys_reference_lvgl_home(ui_);
    else if (key == pxadb::TestControlKey::kBack) (void)pxsys_reference_lvgl_back(ui_);
    else if (audio_ready_) {
        int value = audio_.volume() + (key == pxadb::TestControlKey::kVolumeUp ? 5 : -5);
        (void)audio_.SetVolume(uint8_t(std::max(0, std::min(100, value))));
        Status();
    }
}
void Board::Loop() {
    uint32_t status_tick = 0, scan_tick = 0;
    for (;;) {
        Lock(0); lv_lock();
        CollectCompletions();
#if PXA_DOIT_S31_PANEL_DIAGNOSTICS
        // Keep comparisons at a stable backlight without changing saved idle settings.
        lv_display_trigger_activity(display_);
#endif
        Input();
        if (surface_dirty_.exchange(false)) lv_obj_invalidate(lv_display_get_screen_active(display_));
        if (lv_tick_elaps(status_tick) >= 1000) {
            status_tick = lv_tick_get(); Status(); (void)pxa_board_performance_refresh();
        }
        if (lv_tick_elaps(scan_tick) >= 10000) {
            scan_tick = lv_tick_get();
            portENTER_CRITICAL(&scan_lock_);
            const auto stats = scan_clock_.TakeStats();
            portEXIT_CRITICAL(&scan_lock_);
            ESP_LOGI(kTag, "Scanout: scans=%lu delayed=%lu max_gap_us=%lu surface_misses=%u",
                static_cast<unsigned long>(stats.scans), static_cast<unsigned long>(stats.delayed),
                static_cast<unsigned long>(stats.max_interval_us), missed_surfaces_);
            missed_surfaces_ = 0;
        }
        const uint32_t delay = lv_timer_handler();
        lv_unlock(); Unlock();
        vTaskDelay(pdMS_TO_TICKS(std::max<uint32_t>(1, std::min<uint32_t>(delay, 10))));
    }
}
void Board::Attach(pxsys_standard_system_t* system, pxsys_reference_lvgl_t* ui) {
    system_ = system; ui_ = ui;
    // CreateSystem holds the display/LVGL locks, so DMA keeps scanning the
    // logo while the newly built desktop is waiting for its first refresh.
    const int64_t remaining = 1200000 - (esp_timer_get_time() - int64_t(boot_visible_us_));
    if (boot_visible_us_ && remaining > 0)
        vTaskDelay(pdMS_TO_TICKS((remaining + 999) / 1000));
    boot_.Hide();
    if (ui_) (void)pxsys_reference_lvgl_set_lock_changed_callback(ui_, this, [](void* c, bool locked) {
        auto* b = static_cast<Board*>(c);
        pxa_esp_surface_set_display_unlocked(!locked);
        (void)pxa_host_set_display_interactive(!locked);
        b->touch_blocked_ = b->pressed_;
        if (b->pressed_) b->CancelPointers();
    });
    Status();
}
void Board::Status() {
    if (!system_) return;
    pxsys_system_status_snapshot_t s;
    pxsys_system_status_snapshot_init(&s);
    s.brightness_supported = true; s.brightness_percent = light_.percent();
    s.volume_supported = audio_ready_; s.volume_percent = audio_.volume();
    auto& wifi = WifiManager::GetInstance();
    const bool connected = wifi_ready_ && wifi.IsConnected();
    s.wifi_supported = wifi_ready_; s.wifi_enabled = wifi_ready_ && wifi_enabled_;
    s.network_connected = s.wifi_connected = connected;
    s.network_type = connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    const int rssi = connected ? wifi.GetRssi() : -100;
    s.network_signal_level = s.wifi_signal_level = !connected ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
    (void)pxa_system_clock_update_status(&s);
    (void)pxsys_system_status_service_update(pxsys_standard_system_status(system_), &s);
}
bool Board::SetWifi(bool enabled) {
    if (!wifi_ready_ || !pxa_board_wifi_set_enabled(enabled)) return false;
    wifi_enabled_ = enabled;
    auto& wifi = WifiManager::GetInstance();
    if (enabled) wifi.StartStation();
    else { wifi.StopConfigAp(); wifi.StopStation(); }
    Status(); return true;
}
bool Board::SetLevel(pxsys_level_control_t type, uint8_t percent) {
    if (type == PXSYS_LEVEL_CONTROL_BRIGHTNESS) {
        light_.SetPercent(percent); ApplyBacklight();
    } else if (type != PXSYS_LEVEL_CONTROL_VOLUME || !audio_ready_ || !audio_.SetVolume(percent)) return false;
    Status(); return true;
}
bool Board::Capture(uint16_t* pixels, size_t count, bool after, pxadb::TestControlCaptureInfo* info) {
    if (!pixels || count < kPixels || !Lock(1500)) return false;
    lv_lock();
    if (after) {
        capture_refresh_ = true;
        lv_obj_invalidate(lv_display_get_screen_active(display_)); lv_refr_now(display_);
        capture_refresh_ = false;
    }
    // No LVGL rendering can run under this lock. Finish pending scans before
    // copying, including ordinary captures whose previous front was reused.
    CollectCompletions();
    while (pending_count_) { WaitScan(); CollectCompletions(); }
    if (frame_valid_) {
        std::memcpy(pixels, frames_[front_], kBytes);
        if (info) {
            info->width = kWidth; info->height = kHeight; info->stride_bytes = kWidth * 2;
            info->frame_id = completed_; info->completed_timestamp_us = completed_us_;
            info->source = "st7102-rgb-completed";
        }
    }
    const bool ready = frame_valid_;
    lv_unlock(); Unlock(); return ready;
}
bool Board::Diagnostics() {
    pxadb::TestControlAdapter adapter;
    adapter.context = this; adapter.width = kWidth; adapter.height = kHeight;
    adapter.max_pointers = doit_s31::kTouchPointers;
    adapter.route_pointer_down = [](void* c) {
        auto* b = static_cast<Board*>(c);
        if (!b->Lock(1000)) return false;
        lv_lock(); const bool accepted = !b->pressed_ && !b->Wake(); lv_unlock(); b->Unlock(); return accepted;
    };
    adapter.inject_pointer = [](void* c, uint8_t id, uint16_t x, uint16_t y, bool down) {
        auto* b = static_cast<Board*>(c);
        if (id >= doit_s31::kTouchPointers || x >= kWidth || y >= kHeight || !b->Lock(1000)) return false;
        const bool accepted = !b->pressed_ && b->samples_.Push(id, x, y, down);
        b->Unlock(); return accepted;
    };
    adapter.cancel_pointer = [](void* c, uint8_t id) {
        auto* b = static_cast<Board*>(c);
        if (!b->Lock(1000)) return false;
        const bool ok = b->samples_.Cancel(id); b->Unlock(); return ok;
    };
    adapter.route_key = [](void* c, pxadb::TestControlKey key) {
        auto* b = static_cast<Board*>(c);
        if (!b->Lock(1000)) return false;
        lv_lock(); b->Key(key); lv_unlock(); b->Unlock(); return true;
    };
    adapter.capture_rgb565 = [](void* c, uint16_t* p, size_t n, bool after, pxadb::TestControlCaptureInfo* info) {
        return static_cast<Board*>(c)->Capture(p, n, after, info);
    };
    adapter.set_input_takeover_callback = [](void* c, pxadb::TestControlTakeoverCallback cb, void* ctx) {
        auto* b = static_cast<Board*>(c);
        if (!b->Lock(1000)) return;
        b->takeover_ = cb; b->takeover_context_ = ctx; b->Unlock();
    };
    adapter.presentation_fps_x10 = [](void*) { return pxa_board_performance_refresh(); };
    const auto result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}
// Includes the DMA scan clock and semaphore storage used by the cache-safe ISR.
DRAM_ATTR Board board;
const pxa_board_port_t port = {
    .struct_size = sizeof(pxa_board_port_t),
    .initialize = [](void*) { return board.Initialize(); },
    .display = [](void*) { return board.display(); },
    .display_profile = [](void*, pxsys_display_profile_t* p) {
        pxsys_display_profile_init(p, kWidth, kHeight); p->density_dpi = 198;
        p->shape = PXSYS_DISPLAY_SHAPE_RECTANGLE;
    },
    .set_network_enabled = [](void*, pxsys_network_type_t type, uint8_t on) {
        return type == PXSYS_NETWORK_WIFI && board.SetWifi(on) ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    },
    .set_level = [](void*, pxsys_level_control_t type, uint8_t value) {
        return board.SetLevel(type, value) ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    },
    .performance_get = [](void*, pxsys_reference_performance_option_t option) { return pxa_board_performance_get(option); },
    .performance_set = [](void*, pxsys_reference_performance_option_t option, bool on) { return pxa_board_performance_set(option, on); },
    .system_ready = [](void*, pxsys_standard_system_t* s, pxsys_reference_lvgl_t* ui) { board.Attach(s, ui); },
    .show_initial_frame = [](void*) { board.InitialFrame(); },
    .configure_diagnostics = [](void*) { return board.Diagnostics(); },
    .capture_displayed_rgb565 = [](void*, uint16_t* p, size_t n) { return board.Capture(p, n, false); },
    .set_idle_dim = [](void*, bool on, uint8_t value) { board.Dim(on, value); },
    .idle_screen_off = [](void*) { board.ScreenOff(); },
    .lock_display = [](void*, uint32_t ms) { return board.Lock(ms); },
    .unlock_display = [](void*) { board.Unlock(); },
};
}
extern "C" bool pxa_board_register_selected(void) { return pxa_board_register(&port); }
