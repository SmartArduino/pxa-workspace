#include "pxa_board.h"
#include "epdiy_config.h"
#include "epdiy_capture.h"
#include <atomic>
#include <cstring>
#include <ctime>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lv_decoder.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <pxa_board_common/boot_screen.h>
#include <pxadb/pxadb_service.h>
#include <pxadb_pointer_samples.h>
#include <wifi_manager.h>
extern "C" {
#include "epd_highlevel.h"
#include "epd_board.h"
#include "tps65185.h"
extern const EpdDisplay_t epdiy_v7_panel;
}

namespace {
using namespace epdiy_v7;
constexpr char kTag[] = "epdiy-v7";
constexpr size_t kPixels = size_t(kWidth) * kHeight;
constexpr size_t kDrawBytes = kWidth * 16 * sizeof(uint16_t);
#ifdef CONFIG_PXA_EPDIY_DITHER
constexpr bool kDither = true;
#else
constexpr bool kDither = false;
#endif
#ifdef CONFIG_PXA_EPDIY_FAST_MONO
constexpr bool kFastMono = true;
#else
constexpr bool kFastMono = false;
#endif

class Board {
public:
    bool Initialize();
    bool Lock(uint32_t ms) {
        return ui_mutex_ && xSemaphoreTakeRecursive(ui_mutex_,
            ms ? pdMS_TO_TICKS(ms) : portMAX_DELAY) == pdTRUE;
    }
    void Unlock() { xSemaphoreGiveRecursive(ui_mutex_); }
    lv_display_t* display() const { return display_; }
    void Attach(pxsys_standard_system_t*, pxsys_reference_lvgl_t*);
    void ShowInitialFrame();
    bool SetWifi(bool enabled);
    bool Brightness(uint8_t percent);
    void Dim(bool enabled, uint8_t percent) { dimmed_ = enabled; dim_percent_ = percent; ApplyLight(); }
    void ScreenOff() {
        screen_on_.store(false);
        pxa_esp_surface_set_display_unlocked(false);
        (void)pxa_host_set_display_interactive(false);
        ApplyLight();
    }
    bool Capture(uint16_t*, size_t, pxadb::TestControlCaptureInfo* = nullptr);
    bool PrepareCapture(bool after_present);
    bool CaptureGray(uint8_t*, size_t, size_t*, pxadb::TestControlCaptureInfo*);
    bool ConfigureDiagnostics();
private:
    void UiLoop();
    void Presenter();
    void PublishStatus();
    void ApplyLight();
    bool Wake();
    static void Flush(lv_display_t*, const lv_area_t*, uint8_t*);
    SemaphoreHandle_t ui_mutex_ = nullptr, frame_mutex_ = nullptr, panel_mutex_ = nullptr;
    TaskHandle_t presenter_ = nullptr;
    lv_display_t* display_ = nullptr;
    lv_indev_t* remote_pointer_ = nullptr;
    esp_lv_decoder_handle_t decoder_ = nullptr;
    pxa_board_common::BootScreen boot_screen_;
    uint8_t* desired_ = nullptr;
    EpdiyHighlevelState hl_{};
    pxa_esp_surface_frame_t surface_{};
    bool surface_acquired_ = false, frame_changed_ = false;
    struct Content { uint64_t frame_id = 0, input_us = 0; uint32_t raster_us = 0; } desired_content_;
    uint64_t desired_generation_ = 0, completed_generation_ = 0;
    uint64_t completed_us_ = 0, completed_frames_ = 0;
    bool panel_known_ = false;
    std::atomic<bool> surface_dirty_{true}, screen_on_{true}, wifi_ready_{false};
    bool wifi_enabled_ = true, light_ready_ = false, dimmed_ = false;
    uint8_t brightness_ = 50, dim_percent_ = 10;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* ui_ = nullptr;
    pxadb::PointerSamples<1> samples_;
    pxadb::TestControlTakeoverCallback takeover_ = nullptr;
    void* takeover_context_ = nullptr;
    uint32_t button_since_ = 0;
    bool button_raw_ = false, button_down_ = false;
};

bool Board::Initialize() {
    ui_mutex_ = xSemaphoreCreateRecursiveMutex();
    frame_mutex_ = xSemaphoreCreateMutex();
    panel_mutex_ = xSemaphoreCreateMutex();
    desired_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, kFrameBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    void* draw = heap_caps_aligned_alloc(64, kDrawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ui_mutex_ || !frame_mutex_ || !panel_mutex_ || !desired_ || !draw) return false;
    std::memset(desired_, 0xff, kFrameBytes);
    // The S3 vector difference renderer uses only 1 KiB even with a 64 KiB
    // LUT. Retain its fast path while freeing 63 KiB of internal SRAM.
    epd_init(&epd_board_v7, &epdiy_v7_panel, EPD_LUT_1K);
    epd_set_vcom(CONFIG_PXA_EPDIY_VCOM_MV);
    hl_ = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    // The panel's native coordinates remain landscape. Rotation happens only
    // while packing LVGL tiles, never twice inside the high-level driver.
    epd_set_rotation(EPD_ROT_LANDSCAPE);
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return uint32_t(esp_timer_get_time() / 1000); });
    display_ = lv_display_create(kWidth, kHeight);
    if (!display_) return false;
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(display_, this);
    lv_display_set_buffers(display_, draw, nullptr, kDrawBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display_, Flush);
    lv_display_add_event_cb(display_, [](lv_event_t* event) {
        auto* self = static_cast<Board*>(lv_event_get_user_data(event));
        if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
            self->surface_acquired_ = pxa_esp_surface_acquire_latest(&self->surface_);
            xSemaphoreTake(self->frame_mutex_, portMAX_DELAY);
            self->frame_changed_ = false;
        } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY) {
            if (self->frame_changed_) {
                ++self->desired_generation_;
                self->desired_content_ = self->surface_acquired_
                    ? Content{self->surface_.frame_id, self->surface_.input_timestamp_us,
                              self->surface_.raster_ready_tick_us} : Content{};
            }
            xSemaphoreGive(self->frame_mutex_);
            if (self->surface_acquired_) {
                pxa_esp_surface_release_frame(self->surface_.lease);
                self->surface_acquired_ = false;
            }
            if (self->frame_changed_ && self->presenter_) xTaskNotifyGive(self->presenter_);
        }
    }, LV_EVENT_ALL, this);
    pxa_esp_surface_set_frame_ready_callback([](void* context) {
        static_cast<Board*>(context)->surface_dirty_.store(true);
    }, this);
    if (esp_lv_decoder_init(&decoder_) != ESP_OK) return false;
    boot_screen_.Show(display_);
    remote_pointer_ = lv_indev_create();
    if (!remote_pointer_) return false;
    lv_indev_set_display(remote_pointer_, display_);
    lv_indev_set_type(remote_pointer_, LV_INDEV_TYPE_POINTER);
    // E-paper drawing is slow; input must still consume a normal 35ms tap.
    // LVGL otherwise inherits the 100ms display refresh period here.
    lv_timer_set_period(lv_indev_get_read_timer(remote_pointer_), 10);
    lv_indev_set_user_data(remote_pointer_, this);
    lv_indev_set_read_cb(remote_pointer_, [](lv_indev_t* input, lv_indev_data_t* data) {
        auto* self = static_cast<Board*>(lv_indev_get_user_data(input));
        bool more = false;
        const auto sample = self->samples_.Read(0, more);
        if (sample.cancel) lv_indev_reset(input, nullptr);
        data->point = {sample.x, sample.y};
        data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = more;
    });
    const gpio_config_t button = {.pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    if (gpio_config(&button) != ESP_OK) return false;
#ifdef CONFIG_PXA_EPDIY_FRONTLIGHT
    const ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = LEDC_TIMER_0,
        .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK};
    if (ledc_timer_config(&timer) != ESP_OK) return false;
    for (int channel = 0; channel < 2; ++channel) {
        const ledc_channel_config_t light = {.gpio_num = channel ? 46 : 3,
            .speed_mode = LEDC_LOW_SPEED_MODE, .channel = ledc_channel_t(channel),
            .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
        if (ledc_channel_config(&light) != ESP_OK) return false;
    }
    light_ready_ = true;
#endif
    if (xTaskCreatePinnedToCore([](void* c) { static_cast<Board*>(c)->Presenter(); },
        "epd_refresh", 6144, this, 3, &presenter_, 1) != pdPASS) return false;
    if (xTaskCreatePinnedToCore([](void* c) { static_cast<Board*>(c)->UiLoop(); },
        "epd_ui", 12288, this, 4, nullptr, 0) != pdPASS) return false;
#ifdef CONFIG_PXA_EPDIY_WIFI
    // Network initialization can allocate and scan; keep it outside LVGL and
    // the EPD feeder tasks so it cannot hold up a waveform or input read.
    if (xTaskCreate([](void* context) {
        auto* self = static_cast<Board*>(context);
        auto& wifi = WifiManager::GetInstance();
        WifiManagerConfig config;
        config.ssid_prefix = "EPDiy-V7"; config.language = "zh-CN";
        if (wifi.Initialize(config)) {
            esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            clock.wait_for_sync = false;
            (void)esp_netif_sntp_init(&clock);
            wifi.StartStation();
            self->wifi_ready_.store(true);
        }
        vTaskDelete(nullptr);
    }, "epd_wifi", 4096, this, 2, nullptr) != pdPASS) return false;
#endif
    ESP_LOGI(kTag, "%dx%d logical, %dx%d native, 16-bit 40-pin, VCOM=-%dmV",
        kWidth, kHeight, kNativeWidth, kNativeHeight, CONFIG_PXA_EPDIY_VCOM_MV);
    return true;
}

void Board::Flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
    auto* self = static_cast<Board*>(lv_display_get_user_data(display));
    auto* pixels = reinterpret_cast<uint16_t*>(data);
    const int width = lv_area_get_width(area), height = lv_area_get_height(area);
    const size_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565) / 2;
    if (self->surface_acquired_)
        Compose(self->surface_, pixels, area->x1, area->y1, width, height, stride);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const int dx = area->x1 + x, dy = area->y1 + y;
        self->frame_changed_ |= PutLogical(self->desired_, dx, dy,
            Gray565(pixels[y * stride + x], dx, dy, kDither), kPortrait, kInverted);
    }
    // Our packed PSRAM snapshot owns the pixels now. No waveform wait and no
    // outstanding Surface/LVGL buffer lease crosses into the refresh worker.
    lv_display_flush_ready(display);
}

void Board::Presenter() {
    unsigned partials = 0;
    int64_t last_start_us = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t wait_us = int64_t(CONFIG_PXA_EPDIY_REFRESH_MS) * 1000 -
            (esp_timer_get_time() - last_start_us);
        if (wait_us > 0) vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
        // Drain notifications before taking the snapshot. Arrivals after this
        // point must remain queued, including changes made during a waveform.
        ulTaskNotifyTake(pdTRUE, 0);
        if (!screen_on_.load()) continue;
        xSemaphoreTake(panel_mutex_, portMAX_DELAY);
        xSemaphoreTake(frame_mutex_, portMAX_DELAY);
        std::memcpy(hl_.front_fb, desired_, kFrameBytes);
        const Content content = desired_content_;
        const uint64_t generation = desired_generation_;
        xSemaphoreGive(frame_mutex_);
        const auto diff = Compare(hl_.front_fb, hl_.back_fb);
        const auto mode = Choose(diff, panel_known_, partials, CONFIG_PXA_EPDIY_CLEAN_EVERY,
                                 CONFIG_PXA_EPDIY_GC16_PERCENT, kFastMono);
        if (mode == RefreshMode::None) {
            completed_generation_ = generation;
            xSemaphoreGive(panel_mutex_);
            continue;
        }
        last_start_us = esp_timer_get_time();
        epd_poweron();
        // The reference poweron callback returns void even when PG times out.
        // Output-enable is asserted only after both PG checks have succeeded.
        bool success = epd_ctrl_state()->ep_output_enable;
        int temperature = 25;
        EpdDrawError error = EPD_DRAW_SUCCESS;
        if (success) {
            const int measured = tps_read_thermistor(I2C_NUM_0);
            if (measured >= -20 && measured <= 70) temperature = measured;
            if (mode == RefreshMode::Clear) {
                // Clear physical pixels first. Keep front_fb (the pending UI)
                // intact, and reset only the assumed previous white state.
                epd_clear();
                std::memset(hl_.back_fb, 0xff, kFrameBytes);
            }
            const EpdDrawMode waveform = mode == RefreshMode::Du ? MODE_DU :
                mode == RefreshMode::Gl16 ? MODE_GL16 : MODE_GC16;
            const EpdRect area = mode == RefreshMode::Clear || mode == RefreshMode::Gc16
                ? epd_full_screen() : EpdRect{diff.x, diff.y, diff.width, diff.height};
            error = epd_hl_update_area(&hl_, waveform, temperature, area);
            success = error == EPD_DRAW_SUCCESS;
        }
        // Always remove the high voltage rails, including every error path.
        epd_poweroff();
        panel_known_ = success;
        if (success) {
            partials = mode == RefreshMode::Clear || mode == RefreshMode::Gc16 ? 0 : partials + 1;
            completed_generation_ = generation;
            completed_us_ = esp_timer_get_time();
            ++completed_frames_;
            pxa_board_performance_note_frame();
            pxa_esp_surface_note_display_frame_presented(completed_us_);
            if (content.frame_id) {
                pxa_esp_surface_note_display_latency(content.frame_id, content.input_us,
                                                      content.raster_us, completed_us_);
                pxa_esp_surface_note_game_frame_presented(content.frame_id, completed_us_);
            }
            ESP_LOGI(kTag, "mode=%s changed=%lu area=%dx%d temp=%d duration=%lldms",
                mode == RefreshMode::Clear ? "clear+GC16" : mode == RefreshMode::Gc16 ? "GC16" :
                mode == RefreshMode::Du ? "DU" : "GL16", (unsigned long)diff.pixels,
                diff.width, diff.height, temperature, (completed_us_ - last_start_us) / 1000);
        } else ESP_LOGE(kTag, "Refresh failed: power/output=%d draw=0x%x; next frame will clear",
                         int(epd_ctrl_state()->ep_output_enable), unsigned(error));
        xSemaphoreGive(panel_mutex_);
        // A bounded retry interval also recovers a first frame when no new UI
        // invalidation arrives. Remain idle after a successful static image.
        if (!success) { vTaskDelay(pdMS_TO_TICKS(2000)); xTaskNotifyGive(presenter_); }
    }
}

void Board::UiLoop() {
    uint32_t status_at = 0;
    for (;;) {
        uint32_t delay = 20;
        if (Lock(50)) {
            lv_lock();
            const uint32_t now = lv_tick_get();
            const bool down = gpio_get_level(GPIO_NUM_0) == 0;
            if (down != button_raw_) { button_raw_ = down; button_since_ = now; }
            if (button_down_ != down && now - button_since_ >= 30) {
                button_down_ = down;
                if (down) {
                    if (takeover_) takeover_(takeover_context_);
                    samples_.Cancel(0);
                    Wake();
                    if (ui_) (void)pxsys_reference_lvgl_home(ui_);
                }
            }
            if (surface_dirty_.exchange(false)) lv_obj_invalidate(lv_display_get_screen_active(display_));
            if (now - status_at >= 1000) { PublishStatus(); status_at = now; }
            delay = std::clamp(lv_timer_handler(), uint32_t(5), uint32_t(20));
            lv_unlock(); Unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

void Board::ShowInitialFrame() {
    if (!Lock(2000)) return;
    lv_lock();
    if (ui_ || system_) boot_screen_.Hide();
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    lv_refr_now(display_);
    lv_unlock(); Unlock();
    // Repeated white frames may not have changed a byte, but the physical
    // panel still needs one initialization after reset.
    xTaskNotifyGive(presenter_);
}

void Board::Attach(pxsys_standard_system_t* system, pxsys_reference_lvgl_t* ui) {
    system_ = system; ui_ = ui;
    boot_screen_.Hide();
    ApplyLight();
    pxsys_theme_snapshot_t theme;
    pxsys_theme_snapshot_init(&theme, PXSYS_COLOR_SCHEME_LIGHT);
    (void)pxsys_theme_snapshot_apply_palette(&theme, PXSYS_THEME_PALETTE_GRAPHITE);
    // Pure endpoints give text the full optical contrast of the panel and
    // allow DU for binary controls. Antialiased edges keep their gray levels.
    for (const auto token : {PXSYS_COLOR_BACKGROUND, PXSYS_COLOR_SURFACE,
                              PXSYS_COLOR_SURFACE_CONTAINER_LOWEST})
        theme.colors[token] = 0xffffffff;
    for (const auto token : {PXSYS_COLOR_TEXT_PRIMARY, PXSYS_COLOR_ON_BACKGROUND,
                              PXSYS_COLOR_ON_SURFACE_VARIANT, PXSYS_COLOR_ACCENT,
                              PXSYS_COLOR_ON_PRIMARY_CONTAINER,
                              PXSYS_COLOR_ON_SECONDARY_CONTAINER,
                              PXSYS_COLOR_ON_TERTIARY_CONTAINER})
        theme.colors[token] = 0xff000000;
    theme.colors[PXSYS_COLOR_ON_ACCENT] = 0xffffffff;
    theme.contrast = PXSYS_CONTRAST_HIGH;
    theme.motion_scale_per_mille = 0;
    (void)pxsys_theme_service_update(pxsys_standard_system_theme(system_), &theme);
    if (ui_) {
        (void)pxsys_reference_lvgl_set_animations_enabled(ui_, false);
        (void)pxsys_reference_lvgl_set_lock_changed_callback(ui_, this, [](void* c, bool locked) {
            auto* self = static_cast<Board*>(c);
            pxa_esp_surface_set_display_unlocked(!locked);
            (void)pxa_host_set_display_interactive(!locked);
            self->samples_.Cancel(0);
            lv_indev_reset(self->remote_pointer_, nullptr);
        });
    }
    PublishStatus();
}

bool Board::Wake() {
    const bool was_off = !screen_on_.exchange(true);
    if (was_off) {
        ApplyLight();
        lv_obj_invalidate(lv_display_get_screen_active(display_));
        xTaskNotifyGive(presenter_);
    }
    lv_display_trigger_activity(display_);
    return was_off;
}

bool Board::SetWifi(bool enabled) {
    if (!wifi_ready_.load()) return false;
    wifi_enabled_ = enabled;
    auto& wifi = WifiManager::GetInstance();
    if (enabled) wifi.StartStation();
    else { wifi.StopConfigAp(); wifi.StopStation(); }
    return true;
}

bool Board::Brightness(uint8_t percent) {
    if (!light_ready_) return false;
    brightness_ = std::min<uint8_t>(100, percent); ApplyLight(); return true;
}
void Board::ApplyLight() {
#ifdef CONFIG_PXA_EPDIY_FRONTLIGHT
    if (!light_ready_) return;
    unsigned level = screen_on_.load() ? brightness_ : 0;
    if (dimmed_) level = std::min<unsigned>(level, dim_percent_);
    const unsigned warm = CONFIG_PXA_EPDIY_FRONTLIGHT_WARM_PERCENT;
    const unsigned duty[2] = {level * warm * 1023 / 10000, level * (100 - warm) * 1023 / 10000};
    for (int i = 0; i < 2; ++i) {
        (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel_t(i), duty[i]);
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel_t(i));
    }
#endif
}

void Board::PublishStatus() {
    if (!system_) return;
    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    status.brightness_supported = light_ready_;
    status.brightness_percent = brightness_;
    status.wifi_supported = wifi_ready_.load(); status.wifi_enabled = wifi_enabled_ && status.wifi_supported;
    if (status.wifi_supported) {
        auto& wifi = WifiManager::GetInstance();
        status.wifi_connected = status.network_connected = wifi.IsConnected();
        status.network_type = status.wifi_connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
        const int rssi = status.wifi_connected ? wifi.GetRssi() : -100;
        status.wifi_signal_level = status.network_signal_level = !status.wifi_connected ? 0 :
            rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
    }
    (void)pxa_system_clock_update_status(&status);
    (void)pxsys_system_status_service_update(pxsys_standard_system_status(system_), &status);
}

bool Board::Capture(uint16_t* pixels, size_t count, pxadb::TestControlCaptureInfo* info) {
    if (!pixels || count < kPixels || xSemaphoreTake(panel_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE)
        return false;
    const bool ready = panel_known_ && completed_frames_;
    if (ready) {
        for (int y = 0; y < kHeight; ++y) for (int x = 0; x < kWidth; ++x)
            pixels[size_t(y) * kWidth + x] = GrayRgb565(GetLogical(hl_.back_fb, x, y, kPortrait, kInverted));
        if (info) {
            info->width = kWidth; info->height = kHeight; info->stride_bytes = kWidth * 2;
            info->frame_id = completed_frames_; info->completed_timestamp_us = completed_us_;
            info->source = "epdiy-v7-completed-gray4";
        }
    }
    xSemaphoreGive(panel_mutex_);
    return ready;
}

bool Board::CaptureGray(uint8_t* bytes, size_t capacity, size_t* count,
                         pxadb::TestControlCaptureInfo* info) {
    if (!bytes || !count || !info ||
        xSemaphoreTake(panel_mutex_, pdMS_TO_TICKS(10000)) != pdTRUE) return false;
    const bool ready = panel_known_ && completed_frames_;
    *count = ready ? CaptureGray4Rle(hl_.back_fb, bytes, capacity, kPortrait, kInverted) : 0;
    if (*count) {
        info->width = kWidth; info->height = kHeight; info->stride_bytes = (kWidth + 1) / 2;
        info->frame_id = completed_frames_; info->completed_timestamp_us = completed_us_;
        info->source = "epdiy-v7-completed-gray4";
    }
    xSemaphoreGive(panel_mutex_);
    return *count != 0;
}

bool Board::PrepareCapture(bool after_present) {
    if (!after_present) return true;
    if (!Lock(2000)) return false;
    lv_lock();
    Wake();
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    lv_refr_now(display_);
    xSemaphoreTake(frame_mutex_, portMAX_DELAY);
    const auto target = desired_generation_;
    xSemaphoreGive(frame_mutex_);
    lv_unlock(); Unlock();
    xTaskNotifyGive(presenter_);
    const auto deadline = esp_timer_get_time() + 15000000;
    do {
        if (xSemaphoreTake(panel_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            const bool ready = panel_known_ && completed_generation_ >= target;
            xSemaphoreGive(panel_mutex_);
            if (ready) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (esp_timer_get_time() < deadline);
    return false;
}

bool Board::ConfigureDiagnostics() {
    pxadb::TestControlAdapter adapter;
    adapter.context = this; adapter.width = kWidth; adapter.height = kHeight;
    adapter.route_pointer_down = [](void* c) {
        auto* self = static_cast<Board*>(c);
        if (!self->Lock(5000)) return false;
        lv_lock(); const bool woke = self->Wake(); lv_unlock(); self->Unlock();
        // A wake-only gesture must not reach a control on the lock screen.
        return !woke;
    };
    adapter.inject_pointer = [](void* c, uint8_t id, uint16_t x, uint16_t y, bool pressed) {
        auto* self = static_cast<Board*>(c);
        if (id || x >= kWidth || y >= kHeight || !self->Lock(5000)) return false;
        const bool accepted = self->samples_.Push(id, x, y, pressed);
        self->Unlock(); return accepted;
    };
    adapter.cancel_pointer = [](void* c, uint8_t id) {
        auto* self = static_cast<Board*>(c);
        if (!self->Lock(5000)) return false;
        const bool accepted = self->samples_.Cancel(id); self->Unlock(); return accepted;
    };
    adapter.route_key = [](void* c, pxadb::TestControlKey key) {
        auto* self = static_cast<Board*>(c);
        if (key != pxadb::TestControlKey::kBack && key != pxadb::TestControlKey::kHome) return false;
        if (!self->Lock(1000)) return false;
        lv_lock(); self->Wake();
        const bool ready = self->ui_ != nullptr;
        if (ready) {
            if (key == pxadb::TestControlKey::kHome) (void)pxsys_reference_lvgl_home(self->ui_);
            else (void)pxsys_reference_lvgl_back(self->ui_);
        }
        lv_unlock(); self->Unlock(); return ready;
    };
    adapter.set_input_takeover_callback = [](void* c, pxadb::TestControlTakeoverCallback cb, void* ctx) {
        auto* self = static_cast<Board*>(c);
        if (!self->Lock(1000)) return;
        self->takeover_ = cb; self->takeover_context_ = ctx; self->Unlock();
    };
    adapter.capture_rgb565 = [](void* c, uint16_t* pixels, size_t count, bool after_present,
                                pxadb::TestControlCaptureInfo* info) {
        auto* self = static_cast<Board*>(c);
        return self->PrepareCapture(after_present) && self->Capture(pixels, count, info);
    };
    adapter.capture_gray4_rle = [](void* c, uint8_t* bytes, size_t capacity, size_t* count,
                                   bool after_present, pxadb::TestControlCaptureInfo* info) {
        auto* self = static_cast<Board*>(c);
        return self->PrepareCapture(after_present) && self->CaptureGray(bytes, capacity, count, info);
    };
    const auto result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}

Board g_board;
const pxa_board_port_t kPort = {
    .struct_size = sizeof(pxa_board_port_t),
    .initialize = [](void*) { return g_board.Initialize(); },
    .display = [](void*) { return g_board.display(); },
    .display_profile = [](void*, pxsys_display_profile_t* out) {
        if (!out) return;
        pxsys_display_profile_init(out, kWidth, kHeight);
        out->density_dpi = CONFIG_PXA_EPDIY_DENSITY_DPI;
        out->refresh_rate_hz = std::max(1, 1000 / CONFIG_PXA_EPDIY_REFRESH_MS);
        out->shape = PXSYS_DISPLAY_SHAPE_RECTANGLE;
    },
    .set_network_enabled = [](void*, pxsys_network_type_t type, uint8_t enabled) {
        return type == PXSYS_NETWORK_WIFI && g_board.SetWifi(enabled)
            ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    },
    .set_level = [](void*, pxsys_level_control_t control, uint8_t value) {
        return control == PXSYS_LEVEL_CONTROL_BRIGHTNESS && g_board.Brightness(value)
            ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    },
    .system_ready = [](void*, pxsys_standard_system_t* sys, pxsys_reference_lvgl_t* ui) { g_board.Attach(sys, ui); },
    .show_initial_frame = [](void*) { g_board.ShowInitialFrame(); },
    .configure_diagnostics = [](void*) { return g_board.ConfigureDiagnostics(); },
    .capture_displayed_rgb565 = [](void*, uint16_t* p, size_t n) { return g_board.Capture(p, n); },
    .set_idle_dim = [](void*, bool enabled, uint8_t value) { g_board.Dim(enabled, value); },
    .idle_screen_off = [](void*) { g_board.ScreenOff(); },
    .lock_display = [](void*, uint32_t ms) { return g_board.Lock(ms); },
    .unlock_display = [](void*) { g_board.Unlock(); },
    .initialize_before_storage = true,
};
}
extern "C" bool pxa_board_register_selected(void) { return pxa_board_register(&kPort); }
