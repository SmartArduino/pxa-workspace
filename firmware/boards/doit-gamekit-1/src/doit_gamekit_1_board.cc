#include "pxa_board.h"
#include "doit_gamekit_1_audio.h"
#include "doit_gamekit_1_battery.h"
#include "doit_gamekit_1_config.h"
#include "doit_gamekit_1_input.h"
#include "doit_gamekit_1_navigation.h"
#include "doit_gamekit_1_rotation.h"
#include "doit_gamekit_1_refresh.h"
#include "doit_gamekit_1_surface.h"
#include "doit_gamekit_1_panel.h"
#include "doit_gamekit_1_touch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/sdmmc_host.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <esp_lv_decoder.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_host.h>
#include <pxa/pxa_esp_storage.h>
#include <pxa/pxa_esp_system_font.h>
#include <diskio_sdmmc.h>
#include <pxa_board_api.h>
#include <pxa_board_common/ledc_backlight.h>
#include <pxa_board_common/rgb_port.h>
#include <pxa_board_common/boot_screen.h>
#include <pxa_board_common/lcd_render_trace.h>
#include <pxadb/pxadb_service.h>
#include <pxadb_pointer_samples.h>
#include <sdmmc_cmd.h>
#include <wifi_manager.h>

// Kconfig omits disabled boolean symbols from sdkconfig.h.
#ifndef CONFIG_PXA_DOIT1_LCD_INVERT
#define CONFIG_PXA_DOIT1_LCD_INVERT 0
#endif

#ifndef CONFIG_PXA_DOIT1_LCD_PSRAM_DMA_DIRECT
#define CONFIG_PXA_DOIT1_LCD_PSRAM_DMA_DIRECT 0
#endif

namespace {
using namespace doit_gamekit_1;
constexpr char kTag[] = "doit_gamekit_1";
constexpr size_t kPixels = kWidth * kHeight;
constexpr size_t kFrameBytes = kPixels * sizeof(uint16_t);
constexpr size_t kOutputAlignment = 64;
// Aligned, immutable PSRAM output frames are read directly by DMA. Keep the
// requested transfer size below S31's hardware cap; the driver may round its
// descriptor capacity. TX cache writeback accepts partial cache lines, so a
// rounded chunk need not share the output buffer's 64-byte base alignment.
constexpr bool kDirectDma = CONFIG_PXA_DOIT1_LCD_PSRAM_DMA_DIRECT;
constexpr size_t kDmaBytes = kNativeWidth * (kDirectDma ? 64 : 4) * sizeof(uint16_t);
#if CONFIG_PXA_DOIT1_LCD_FLIP_180
constexpr auto kRotation = LandscapeRotation::Deg90;
#else
constexpr auto kRotation = LandscapeRotation::Deg270;
#endif
constexpr auto kTouchRotation = kRotation;
constexpr char kPanelName[] = "ST7789V TN";
constexpr char kCaptureSource[] = "st7789-sw-rotation-completed";
static_assert(kFrameBytes % kOutputAlignment == 0);
static_assert(kControllerMask == PXA_HOST_CONTROLLER_BUTTON_MASK);
static_assert(X == PXA_HOST_CONTROLLER_X && R == PXA_HOST_CONTROLLER_R);

class Gamekit {
public:
    bool Initialize();
    bool LockDisplay(uint32_t timeout_ms) {
        return display_mutex_ && xSemaphoreTakeRecursive(display_mutex_,
            timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) == pdTRUE;
    }
    void UnlockDisplay() { xSemaphoreGiveRecursive(display_mutex_); }
    void AttachSystem(pxsys_standard_system_t* system, pxsys_reference_lvgl_t* ui);
    void ShowInitialFrame();
    bool SetWifi(bool enabled);
    void SetVolume(uint8_t value);
    void SetBrightness(uint8_t value) { backlight_.SetPercent(value); ApplyBacklight(); }
    void SetIdleDim(bool enabled, uint8_t value) {
        backlight_.SetIdleDim(enabled, value); ApplyBacklight();
    }
    void ScreenOff() {
        screen_on_ = false;
        pxa_esp_surface_set_display_unlocked(false);
        (void)pxa_host_set_display_interactive(false);
        ApplyBacklight();
    }
    bool Capture(uint16_t* pixels, size_t count, pxadb::TestControlCaptureInfo* info = nullptr);
    bool ConfigureDiagnostics();
    lv_display_t* display() const { return display_; }

private:
    bool InitializeDisplay();
    bool InitializeButtons();
    bool InitializeTouch();
    void PollTouch(uint32_t now);
    void CancelTouch();
    bool CreateRemotePointer(uint8_t id);
    bool InitializeSd();
    bool InitializeWifi();
    void ProvisionWifi();
    void PollInput();
    void ReportController(uint32_t buttons, uint32_t now);
    void Back();
    lv_obj_t* NavigationScope() const;
    void PollPresentation();
    void DrawPerformanceOverlay(uint16_t* native);
    void SubmitFrames();
    uint8_t AcquireOutput();
    void QueueOutput(uint8_t index, bool content, const pxa_esp_surface_frame_t* frame = nullptr);
    void PublishStatus();
    void ApplyBacklight() { backlight_.Apply(screen_on_ && frame_ready_.load()); }
    static void Flush(lv_display_t*, const lv_area_t*, uint8_t*);

    DoitGamekit1Audio audio_;
    DoitGamekit1Battery battery_;
    std::atomic<bool> audio_ready_{false}, wifi_ready_{false};
    std::atomic<bool> sntp_ready_{false};
    std::atomic<bool> wifi_enabled_{true};
    bool screen_on_ = true, swallow_until_release_ = false, backlight_started_ = false;
    pxa_board_common::LedcBacklight backlight_;
    pxa_board_common::BootScreen boot_screen_;
    DoitGamekit1Panel panel_;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    esp_lv_decoder_handle_t decoder_ = nullptr;
    SemaphoreHandle_t display_mutex_ = nullptr;
    lv_display_t* display_ = nullptr;
    lv_indev_t* pointer_ = nullptr;
    lv_obj_t* cursor_ = nullptr;
    uint16_t* frame_ = nullptr;
    pxa::board::LcdRenderTrace ui_trace_;
    pxa::board::LcdTransferTrace transfer_trace_;
    uint16_t* native_frames_[2]{};
    struct Output {
        uint8_t index; bool content;
        uint64_t frame_id, input_timestamp_us;
        uint32_t raster_ready_tick_us;
    };
    StaticQueue_t free_queue_storage_{}, ready_queue_storage_{};
    uint8_t free_items_[2]{}, ready_items_[2 * sizeof(Output)]{};
    QueueHandle_t free_outputs_ = nullptr, ready_outputs_ = nullptr;
    SemaphoreHandle_t dma_done_ = nullptr;
    std::atomic<uint32_t> pending_outputs_{0};
    std::atomic<int> completed_output_{-1};
    std::atomic<bool> frame_ready_{false};
    std::atomic<TaskHandle_t> ui_task_{nullptr};
    bool direct_active_ = false;
    std::atomic<bool> surface_dirty_{true};
    pxa_esp_surface_frame_t surface_{};
    bool surface_acquired_ = false;
    std::atomic<uint32_t> completed_frames_{0}, completed_us_low_{0};
    std::atomic<uint32_t> completed_content_frames_{0};
    RefreshTracker refresh_;
    uint32_t sampled_content_frames_ = 0, overlay_fps_ = UINT32_MAX;
    bool overlay_enabled_ = false;
    uint32_t flush_count_ = 0;
    static constexpr uint8_t kDebugPointers = 5;
    QueueHandle_t remote_keys_ = nullptr;
    lv_indev_t* remote_pointers_[kDebugPointers]{};
    pxadb::PointerSamples<kDebugPointers> remote_samples_;
    DoitGamekit1Touch touch_;
    bool touch_ready_ = false, touch_pressed_ = false, touch_blocked_ = false;
    bool touch_mode_ = false;
    uint32_t touch_tick_ = 0, touch_success_tick_ = 0;
    TouchSlots touch_slots_;
    std::array<TouchPoint, 2> touch_points_{};
    lv_indev_t* touch_pointers_[2]{};
    pxadb::PointerSamples<2> touch_samples_;
    portMUX_TYPE remote_lock_ = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> physical_active_{false}, wake_requested_{false};
    // Bits 16/17 mark a diagnostic override and its connection state.
    std::atomic<uint32_t> remote_controller_{0};
    bool reported_connected_ = true;
    pxadb::TestControlTakeoverCallback takeover_ = nullptr;
    void* takeover_context_ = nullptr;
    uint32_t previous_raw_ = 0;
    pxsys_standard_system_t* system_ = nullptr;
    pxsys_reference_lvgl_t* ui_ = nullptr;
    Debouncer debounce_;
    Shortcuts shortcuts_;
    KeyNavigation navigation_;
    bool force_pointer_ = false, pointer_mode_ = false, focus_mode_ = true, pointer_down_ = false;
    bool gamepad_consumed_ = false;
    uint32_t previous_delivered_ = 0;
    int cursor_x_ = kWidth / 2, cursor_y_ = kHeight / 2;
    uint32_t input_tick_ = 0, report_tick_ = 0, reported_buttons_ = 0;
};

bool Gamekit::Initialize() {
    // V0.8 schematic, sheet 1: GPIO47 sinks LED2 through R47 to 3.3 V.
    // Reserve the physical line for this logical indicator; it is no longer L.
    gpio_config_t indicator_config{};
    indicator_config.pin_bit_mask = UINT64_C(1) << 47;
    indicator_config.mode = GPIO_MODE_OUTPUT;
    if (gpio_set_level(GPIO_NUM_47, 1) != ESP_OK || gpio_config(&indicator_config) != ESP_OK ||
        !pxa_board_binary_indicator_initialize(nullptr, [](void*, bool on) {
            return gpio_set_level(GPIO_NUM_47, on ? 0 : 1) == ESP_OK;
        })) ESP_LOGW(kTag, "Status indicator application interface unavailable");
    if (!InitializeDisplay()) return false;
    ShowInitialFrame();
    if (!InitializeButtons()) return false;
#if CONFIG_PXA_DOIT1_TOUCH
    touch_ready_ = InitializeTouch();
    if (!touch_ready_) ESP_LOGW(kTag, "FT6336U unavailable; button navigation remains active");
#endif
    // S31 ADC conversions and Wi-Fi PHY temperature reads nest the RTC and
    // analog register spinlocks in opposite orders. Run both on the Wi-Fi
    // core so their interrupt-masked critical sections cannot overlap across
    // cores (otherwise even a background battery sample can deadlock Wi-Fi).
#if CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1
    constexpr BaseType_t battery_core = 1;
#else
    constexpr BaseType_t battery_core = 0;
#endif
    if (xTaskCreatePinnedToCore([](void* context) {
            auto* self = static_cast<Gamekit*>(context);
            ESP_LOGI(kTag, "Battery worker started, core=%d", xPortGetCoreID());
            if (!self->battery_.Initialize(CONFIG_PXA_DOIT1_BATTERY_R37_KOHM,
                                            CONFIG_PXA_DOIT1_BATTERY_R52_KOHM)) {
                vTaskDelete(nullptr);
                return;
            }
            for (;;) {
                self->battery_.Poll(uint32_t(esp_timer_get_time() / 1000));
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }, "gamekit_battery", 3072, this, 1, nullptr, battery_core) != pdPASS)
        ESP_LOGW(kTag, "Battery worker unavailable");
    // Own the LVGL clock and timer loop for the custom SPI display. The
    // monotonic clock also keeps input timestamps and idle policy in one domain.
    if (xTaskCreatePinnedToCore([](void* context) {
            auto* self = static_cast<Gamekit*>(context);
            ESP_LOGI(kTag, "LVGL loop started, core=%d tick=%lu",
                     xPortGetCoreID(), static_cast<unsigned long>(lv_tick_get()));
            self->ui_task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
            bool first = true;
            for (;;) {
                uint32_t delay = 5;
                if (self->LockDisplay(20)) {
                    if (first) ESP_LOGI(kTag, "Entering first LVGL timer handler");
                    lv_lock();
                    self->PollPresentation();
                    lv_unlock();
                    delay = std::min(lv_timer_handler(), uint32_t(10));
                    if (first) ESP_LOGI(kTag, "First LVGL timer handler returned");
                    first = false;
                    self->UnlockDisplay();
                }
                // A new Surface wakes presentation immediately instead of waiting
                // for a second 16 ms LVGL polling timer.
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::max(delay, uint32_t(1))));
            }
        /* Compose/rotate the previous Surface while the Guest on core 1
         * emulates the next frame. Output queue ownership still fences DMA. */
        }, "gamekit_lvgl", 12288, this, 4, nullptr, 0) != pdPASS) return false;
    // Product LittleFS has already mounted. Nest the removable volume below
    // it so the existing Files UI and PXADB can access /sdcard without a new
    // storage ABI or replacing installed apps, fonts and private saves.
    if (!InitializeSd()) ESP_LOGW(kTag, "No usable SD card; internal PXA storage remains available");
    audio_ready_ = audio_.Initialize();
    if (!audio_ready_) ESP_LOGW(kTag, "MAX98357A output unavailable");
    wifi_ready_ = InitializeWifi();
    if (!wifi_ready_) ESP_LOGW(kTag, "Wi-Fi unavailable");
    ESP_LOGI(kTag, "%s: 320x240 %s, 14 keys, SDMMC, FT6336U=%s",
             kName, kPanelName, touch_ready_ ? "ready" : "unavailable");
    return true;
}

bool Gamekit::InitializeDisplay() {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t light = {
        .gpio_num = kLcdBacklight, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    if (ledc_timer_config(&timer) != ESP_OK || ledc_channel_config(&light) != ESP_OK)
        return false;
    backlight_.Configure(LEDC_CHANNEL_0, 1023, 75);

    const spi_bus_config_t bus = {
        .mosi_io_num = kLcdMosi, .miso_io_num = -1, .sclk_io_num = kLcdClock,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = int(kDmaBytes),
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = gpio_num_t(kLcdCs), .dc_gpio_num = gpio_num_t(kLcdDc), .spi_mode = 0,
        .pclk_hz = CONFIG_PXA_DOIT1_LCD_CLOCK_HZ, .trans_queue_depth = 4,
        .user_ctx = this,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .flags = {.psram_dma_direct = kDirectDma},
    };
    if (esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t(SPI2_HOST), &io, &io_) != ESP_OK ||
        !panel_.Initialize(io_))
        return false;

    // Query the SPI master only after the LCD IO has registered its device.
    size_t dma_chunk_bytes = 0;
    if (spi_bus_get_max_transaction_len(SPI2_HOST, &dma_chunk_bytes) != ESP_OK ||
        (kDirectDma && dma_chunk_bytes < kDmaBytes)) {
        ESP_LOGE(kTag, "LCD DMA capacity is smaller than the requested chunk");
        return false;
    }
    ESP_LOGI(kTag, "LCD DMA: direct_psram=%d chunk=%u frame=%u alignment=%u",
             kDirectDma, unsigned(dma_chunk_bytes), unsigned(kFrameBytes),
             unsigned(kOutputAlignment));

    dma_done_ = xSemaphoreCreateBinary();
    frame_ = static_cast<uint16_t*>(heap_caps_aligned_calloc(kOutputAlignment, kPixels, sizeof(uint16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto& native : native_frames_)
        native = static_cast<uint16_t*>(heap_caps_aligned_alloc(kOutputAlignment, kFrameBytes,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    free_outputs_ = xQueueCreateStatic(2, sizeof(uint8_t), free_items_, &free_queue_storage_);
    ready_outputs_ = xQueueCreateStatic(2, sizeof(Output), ready_items_, &ready_queue_storage_);
    // LVGL draws directly into the retained logical PSRAM frame, as on
    // PAI Touch. Small partial buffers repeatedly walk text/shadow draw tasks.
    // SPI reads separate rotated outputs, so no second logical buffer is needed.
    if (!dma_done_ || !frame_ || !native_frames_[0] || !native_frames_[1] ||
        !free_outputs_ || !ready_outputs_) return false;
    for (uint8_t index = 0; index < 2; ++index) xQueueSend(free_outputs_, &index, 0);
    if (xTaskCreatePinnedToCore([](void* context) {
            static_cast<Gamekit*>(context)->SubmitFrames();
        }, "gamekit_lcd", 4096, this, 5, nullptr, 0) != pdPASS) return false;
    display_mutex_ = xSemaphoreCreateRecursiveMutex();
    if (!display_mutex_) return false;
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return uint32_t(esp_timer_get_time() / 1000); });
    if (!LockDisplay(1000)) return false;
    lv_lock();
    display_ = lv_display_create(kWidth, kHeight);
    if (display_) {
        lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
        lv_display_set_user_data(display_, this);
        lv_display_set_buffers(display_, frame_, nullptr, kFrameBytes, LV_DISPLAY_RENDER_MODE_DIRECT);
        if (!lv_display_get_buf_active(display_)) {
            ESP_LOGE(kTag, "LVGL rejected the aligned SPI draw buffers");
            lv_unlock();
            UnlockDisplay();
            return false;
        }
        lv_display_set_flush_cb(display_, Flush);
        boot_screen_.Show(display_);
        lv_display_add_event_cb(display_, [](lv_event_t* event) {
            auto* self = static_cast<Gamekit*>(lv_event_get_user_data(event));
            if (lv_event_get_code(event) == LV_EVENT_INVALIDATE_AREA) {
                self->refresh_.Invalidated();
                if (self->direct_active_) {
                    // Direct scanout bypasses frame_, so its untouched tiles
                    // are stale. The first trusted overlay must compose the
                    // whole current Surface, not just its own dirty strip.
                    auto* area = static_cast<lv_area_t*>(lv_event_get_param(event));
                    *area = {0, 0, kWidth - 1, kHeight - 1};
                }
            } else if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
                self->ui_trace_.Begin();
                self->direct_active_ = false;
                self->refresh_.BeginRender();
                self->surface_acquired_ = pxa_esp_surface_acquire_latest(&self->surface_);
            } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY) {
                self->ui_trace_.End(kTag, self->surface_acquired_, true);
                self->refresh_.EndRender();
                if (self->surface_acquired_) {
                    // DMA reads our rotated output frame, never the borrowed Surface.
                    pxa_esp_surface_release_frame(self->surface_.lease);
                    self->surface_acquired_ = false;
                }
            }
        }, LV_EVENT_ALL, this);
        pxa_esp_surface_set_frame_ready_callback([](void* context) {
            auto* self = static_cast<Gamekit*>(context);
            self->surface_dirty_.store(true, std::memory_order_release);
            if (const auto task = self->ui_task_.load(std::memory_order_acquire)) xTaskNotifyGive(task);
        }, this);
        (void)pxa_esp_game_render_set_scale_profile(
            PXA_GAME_RENDER_SCALE_MASK_1X | PXA_GAME_RENDER_SCALE_MASK_2X |
                PXA_GAME_RENDER_SCALE_MASK_4X, PXA_GAME_RENDER_SCALE_1X);
    }
    const bool ready = display_ && esp_lv_decoder_init(&decoder_) == ESP_OK;
    lv_unlock();
    UnlockDisplay();
    ESP_ERROR_CHECK(panel_.DisplayOn());
    ESP_LOGI(kTag, "Native portrait, software rotation: logical=%dx%d output=%dx%d, "
             "tiled software rotation, double-buffered %d MHz SPI, no TE",
             kWidth, kHeight, kNativeWidth, kNativeHeight,
             CONFIG_PXA_DOIT1_LCD_CLOCK_HZ / 1000000);
    // The backlight stays off until a complete frame has reached the panel.
    return ready;
}

void Gamekit::ShowInitialFrame() {
    if (!display_ || !LockDisplay(2000)) return;
    lv_lock();
    if (backlight_started_) boot_screen_.Hide();
    lv_obj_invalidate(lv_display_get_screen_active(display_));
    lv_refr_now(display_);
    lv_unlock();
    // The SPI worker owns the output buffers and does not need the UI lock.
    // Enable the backlight only once the entire first frame has reached GRAM.
    while (pending_outputs_.load(std::memory_order_acquire)) {
        if (xSemaphoreTake(dma_done_, pdMS_TO_TICKS(1000)) != pdTRUE) break;
    }
    if (!backlight_started_ && frame_ready_.load(std::memory_order_acquire)) {
        backlight_started_ = true;
        ApplyBacklight();
        ESP_LOGI(kTag, "Boot frame visible at %lu ms", static_cast<unsigned long>(esp_log_timestamp()));
    }
    UnlockDisplay();
}

void Gamekit::Flush(lv_display_t* display, const lv_area_t* area, uint8_t* bytes) {
    auto* self = static_cast<Gamekit*>(lv_display_get_user_data(display));
    if (++self->flush_count_ <= 2)
        ESP_LOGI(kTag, "LCD flush %lu: (%ld,%ld)-(%ld,%ld), last=%d",
                 static_cast<unsigned long>(self->flush_count_), long(area->x1), long(area->y1),
                 long(area->x2), long(area->y2), int(lv_display_flush_is_last(display)));
    // DIRECT mode supplies the full framebuffer base even for a dirty rectangle.
    auto* pixels = reinterpret_cast<uint16_t*>(bytes) + area->y1 * kWidth + area->x1;
    const int width = lv_area_get_width(area), height = lv_area_get_height(area);
    constexpr size_t stride = kWidth;
    self->ui_trace_.Tile();
    auto phase = self->ui_trace_.Mark();
    if (self->surface_acquired_)
        Compose(self->surface_, pixels, area->x1, area->y1, width, height, stride);
    // The virtual cursor must remain above a composed game and trusted UI.
    if (self->pointer_mode_) {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            const int dx = area->x1 + x - self->cursor_x_ - 4;
            const int dy = area->y1 + y - self->cursor_y_ - 4;
            const int radius = dx * dx + dy * dy;
            if (radius <= 16) pixels[y * stride + x] = radius >= 9 ? 0 : 0xffff;
        }
    }
    self->ui_trace_.Add(pxa::board::LcdRenderTrace::Compose, phase);
    // Dirty pixels already live at their final logical offsets in frame_.
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }
    phase = self->ui_trace_.Mark();
    const uint8_t output = self->AcquireOutput();
    self->ui_trace_.Add(pxa::board::LcdRenderTrace::Wait, phase);
    phase = self->ui_trace_.Mark();
    RotateRgb565(self->frame_, kWidth, self->native_frames_[output], kWidth, kHeight, kRotation);
    self->ui_trace_.Add(pxa::board::LcdRenderTrace::Rotate, phase);
    self->QueueOutput(output, self->refresh_.content(),
                      self->surface_acquired_ ? &self->surface_ : nullptr);
    // The source and LVGL tiles are no longer borrowed. SPI owns a separate
    // native output until the submit task has drained every queued chunk.
    lv_display_flush_ready(display);
}

uint8_t Gamekit::AcquireOutput() {
    uint8_t index;
    xQueueReceive(free_outputs_, &index, portMAX_DELAY);
    return index;
}

void Gamekit::QueueOutput(uint8_t index, bool content, const pxa_esp_surface_frame_t* frame) {
    DrawPerformanceOverlay(native_frames_[index]);
    const Output output{index, content, frame ? frame->frame_id : 0,
        frame ? frame->input_timestamp_us : 0, frame ? frame->raster_ready_tick_us : 0};
    pending_outputs_.fetch_add(1, std::memory_order_release);
    xQueueSend(ready_outputs_, &output, portMAX_DELAY);
}

void Gamekit::SubmitFrames() {
    Output output;
    for (;;) {
        xQueueReceive(ready_outputs_, &output, portMAX_DELAY);
        const uint64_t transfer_started = transfer_trace_.Begin();
        const auto result = panel_.Draw(native_frames_[output.index]);
        // A no-command parameter transaction drains SPI's queue, including
        // partial submissions on error. Never recycle a buffer before this.
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, -1, nullptr, 0));
        transfer_trace_.End(kTag, transfer_started, result == ESP_OK);
        if (result == ESP_OK) {
            completed_output_.store(output.index, std::memory_order_release);
            const uint64_t completed_us = esp_timer_get_time();
            completed_us_low_.store(uint32_t(completed_us));
            pxa_esp_surface_note_display_frame_presented(completed_us);
            if (output.frame_id) {
                pxa_esp_surface_note_display_latency(output.frame_id, output.input_timestamp_us,
                                                     output.raster_ready_tick_us, completed_us);
                pxa_esp_surface_note_game_frame_presented(output.frame_id, completed_us);
            }
            completed_frames_.fetch_add(1, std::memory_order_release);
            if (output.content) completed_content_frames_.fetch_add(1, std::memory_order_release);
            frame_ready_.store(true, std::memory_order_release);
        } else ESP_LOGE(kTag, "LCD submission failed: %s", esp_err_to_name(result));
        xQueueSend(free_outputs_, &output.index, portMAX_DELAY);
        pending_outputs_.fetch_sub(1, std::memory_order_release);
        xSemaphoreGive(dma_done_);
    }
}

bool Gamekit::Capture(uint16_t* pixels, size_t count, pxadb::TestControlCaptureInfo* info) {
    if (!pixels || count < kPixels || !frame_ready_.load() || !LockDisplay(1000)) {
        ESP_LOGW(kTag, "Capture unavailable: ready=%d flushing=%d completed=%lu",
                 int(frame_ready_.load()), int(pending_outputs_.load()),
                 static_cast<unsigned long>(completed_frames_.load()));
        return false;
    }
    lv_lock();
    const auto deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (pending_outputs_.load(std::memory_order_acquire) &&
           int32_t(deadline - xTaskGetTickCount()) > 0)
        xSemaphoreTake(dma_done_, pdMS_TO_TICKS(10));
    const int output = completed_output_.load(std::memory_order_acquire);
    const bool complete = !pending_outputs_.load(std::memory_order_acquire) && output >= 0;
    if (complete) {
        CaptureRotatedRgb565(native_frames_[output], pixels, kWidth, kHeight, kRotation);
        if (info) {
            info->width = kWidth; info->height = kHeight; info->stride_bytes = kWidth * 2;
            info->frame_id = completed_frames_.load();
            const uint64_t now = esp_timer_get_time();
            info->completed_timestamp_us = now - uint32_t(uint32_t(now) - completed_us_low_.load());
            info->source = kCaptureSource;
        }
    }
    lv_unlock();
    UnlockDisplay();
    return complete;
}

void Gamekit::PollPresentation() {
    // NVS, logging and FPS sampling belong on the UI task, never in the DMA ISR.
    const uint32_t completed = completed_content_frames_.load(std::memory_order_acquire);
    while (sampled_content_frames_ != completed) {
        ++sampled_content_frames_;
        pxa_board_performance_note_frame();
    }
    const bool content = surface_dirty_.exchange(false, std::memory_order_acq_rel);
    bool diagnostic = false;
    const uint32_t fps = pxa_board_performance_refresh();
    const bool enabled = pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY);
    if (screen_on_ && frame_ready_.load(std::memory_order_acquire) &&
        (enabled != overlay_enabled_ || (enabled && fps != overlay_fps_))) {
        overlay_enabled_ = enabled;
        overlay_fps_ = fps;
        constexpr int left = (kWidth - PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH) / 2;
        const lv_area_t area = {left, 0, left + PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH - 1,
                               PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT - 1};
        if (direct_active_) diagnostic = true;
        else {
            refresh_.BeginDiagnosticInvalidation();
            lv_obj_invalidate_area(lv_display_get_screen_active(display_), &area);
            refresh_.EndDiagnosticInvalidation();
        }
    }
    if (!content && !diagnostic) return;
    pxa_esp_surface_frame_t frame{};
    // Leave the mailbox to LVGL when a trusted overlay or pointer needs it.
    // Releasing a GuestMapped frame here would return its pixels to the Guest
    // before the LVGL refresh could compose them. The direct acquisition also
    // enforces the fresh-frame barrier after closing a modal or Recents.
    const bool composition = pointer_mode_ || pxa_esp_surface_composition_required();
    const bool acquired = !composition && pxa_esp_surface_acquire_latest_for_direct(&frame);
    const bool direct = acquired && CanPresentDirectly(frame, false, false);
    if (direct) {
        const uint8_t output = AcquireOutput();
        RotateScaledRgb565(reinterpret_cast<const uint16_t*>(frame.pixels), frame.stride_bytes / 2,
                          native_frames_[output], frame.width, frame.height,
                          kWidth / frame.width, kRotation);
        // Release the runtime's borrowed frame before waiting on SPI.
        pxa_esp_surface_release_frame(frame.lease);
        QueueOutput(output, content, &frame);
    } else {
        if (acquired) pxa_esp_surface_release_frame(frame.lease);
        lv_obj_invalidate(lv_display_get_screen_active(display_));
    }
    direct_active_ = direct;
}

void Gamekit::DrawPerformanceOverlay(uint16_t* native) {
    if (!pxa_board_performance_get(PXSYS_REFERENCE_PERFORMANCE_OVERLAY)) return;
    constexpr int width = PXA_BOARD_PERFORMANCE_OVERLAY_WIDTH;
    constexpr int height = PXA_BOARD_PERFORMANCE_OVERLAY_HEIGHT;
    constexpr int left = (kWidth - width) / 2;
    uint16_t patch[width * height]{};
    pxa_board_performance_draw_rgb565(patch, kWidth, kHeight, width,
                                     left, 0, width, height, false);
    RotateRgb565Patch(patch, width, native, kWidth, kHeight,
                     left, 0, width, height, kRotation);
}

bool Gamekit::InitializeSd() {
    const auto* board=pxa_board_current();
    const bool primary=CONFIG_PXA_STORAGE_PARTITION_LABEL[0] &&
        (esp_littlefs_mounted(CONFIG_PXA_STORAGE_PARTITION_LABEL) ||
         (board && board->initialize_before_storage && esp_partition_find_first(
             ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,CONFIG_PXA_STORAGE_PARTITION_LABEL)));
    const char* mount=primary?CONFIG_PXA_MOUNT_POINT "/sdcard":CONFIG_PXA_MOUNT_POINT;
    if(primary && mkdir(mount,0775)!=0 && errno!=EEXIST && errno!=ENOENT)return false;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = gpio_num_t(kSdClock);
    slot.cmd = gpio_num_t(kSdCmd);
    slot.d0 = gpio_num_t(kSdD0);
    slot.d1 = gpio_num_t(kSdD1);
    slot.d2 = gpio_num_t(kSdD2);
    slot.d3 = gpio_num_t(kSdD3);
    slot.width = 4;
    const esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false, .max_files = 8, .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t* card = nullptr;
    const auto error = esp_vfs_fat_sdmmc_mount(mount, &host, &slot, &config, &card);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "SD mount: %s (card is never auto-formatted)", esp_err_to_name(error));
        return false;
    }
    ESP_LOGI(kTag, "SD mounted at %s: %llu MiB, SDMMC slot 0, 4-bit 20 MHz", mount,
             static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    char drive[8];
    std::snprintf(drive, sizeof(drive), "%u:", unsigned(ff_diskio_get_pdrv_card(card)));
    DWORD free_clusters = 0;
    FATFS* filesystem = nullptr;
    uint32_t allocation_unit = 0;
    if (f_getfree(drive, &free_clusters, &filesystem) == FR_OK && filesystem) {
#if FF_MAX_SS != FF_MIN_SS
        allocation_unit = uint32_t(filesystem->csize) * filesystem->ssize;
#else
        allocation_unit = uint32_t(filesystem->csize) * FF_MIN_SS;
#endif
    }
    uint64_t total = 0, free = 0;
    const bool space_known = esp_vfs_fat_info(mount, &total, &free) == ESP_OK;
    const auto storage = pxa_esp_storage_register_fat(mount, "SD card", &card->cid,
        sizeof(card->cid), total, free, space_known ? 3 : 0, allocation_unit);
    if (storage != PXA_STATUS_OK)
        ESP_LOGW(kTag, "SD data volume registration: %ld", long(storage));
    else {
        const auto media = pxa_esp_storage_select_app_media(mount);
        if (media != PXA_STATUS_OK)
            ESP_LOGW(kTag, "Fixed app media volume: %ld", long(media));
        ESP_LOGI(kTag, "SD metadata ready at %lu ms", static_cast<unsigned long>(esp_log_timestamp()));
        if (pxa_esp_system_font_prepare(mount, true))
            ESP_LOGI(kTag, "Verified system font: %s", pxa_esp_system_font_path());
    }
    return true;
}

// Called with the display lock held. Additional diagnostic fingers are lazy,
// so ordinary controller play retains the same single pointer timer.
bool Gamekit::CreateRemotePointer(uint8_t id) {
    if (id >= kDebugPointers) return false;
    if (remote_pointers_[id]) return true;
    auto* pointer = lv_indev_create();
    if (!pointer) return false;
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_disp(pointer, display_);
    lv_indev_set_driver_data(pointer, this);
    lv_indev_set_user_data(pointer, reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    lv_indev_set_read_cb(pointer, [](lv_indev_t* device, lv_indev_data_t* data) {
        auto* self = static_cast<Gamekit*>(lv_indev_get_driver_data(device));
        const auto id = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_indev_get_user_data(device)));
        bool more = false;
        portENTER_CRITICAL(&self->remote_lock_);
        const auto sample = self->remote_samples_.Read(id, more);
        portEXIT_CRITICAL(&self->remote_lock_);
        if (sample.pressed) self->touch_mode_ = true;
        if (sample.cancel) lv_indev_wait_release(device);
        data->point = {sample.x, sample.y};
        data->timestamp = lv_tick_get();
        data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = more;
    });
    lv_timer_set_period(lv_indev_get_read_timer(pointer), 5);
    remote_pointers_[id] = pointer;
    return true;
}

bool Gamekit::InitializeButtons() {
    uint64_t mask = 0;
    for (int pin : kButtonPins) mask |= 1ULL << pin;
    const gpio_config_t config = {
        .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    remote_keys_ = xQueueCreate(8, sizeof(pxadb::TestControlKey));
    if (!remote_keys_ || gpio_config(&config) != ESP_OK || !LockDisplay(1000))
        return false;
    lv_lock();
    const bool remote_ready = CreateRemotePointer(0);
    pointer_ = lv_indev_create();
    if (pointer_) {
        lv_indev_set_type(pointer_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(pointer_, display_);
        lv_indev_set_user_data(pointer_, this);
        lv_indev_set_read_cb(pointer_, [](lv_indev_t* device, lv_indev_data_t* data) {
            auto* self = static_cast<Gamekit*>(lv_indev_get_user_data(device));
            data->point = {self->cursor_x_, self->cursor_y_};
            data->timestamp = lv_tick_get();
            data->state = self->pointer_down_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        });
        lv_timer_set_period(lv_indev_get_read_timer(pointer_), 5);
        cursor_ = lv_obj_create(lv_display_get_layer_sys(display_));
        lv_obj_remove_style_all(cursor_);
        lv_obj_set_size(cursor_, 9, 9);
        lv_obj_set_style_bg_color(cursor_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cursor_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cursor_, lv_color_black(), 0);
        lv_obj_set_style_border_width(cursor_, 1, 0);
        lv_obj_set_style_radius(cursor_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_clickable(cursor_, false);
        lv_obj_set_scrollable(cursor_, false);
        lv_obj_set_hidden(cursor_, true);
        lv_indev_set_cursor(pointer_, cursor_);
    }
    auto* poll = lv_timer_create([](lv_timer_t* timer) {
        static_cast<Gamekit*>(lv_timer_get_user_data(timer))->PollInput();
    }, 5, this);
    auto* status = lv_timer_create([](lv_timer_t* timer) {
        static_cast<Gamekit*>(lv_timer_get_user_data(timer))->PublishStatus();
    }, 1000, this);
    const bool navigation_ready = navigation_.Initialize(display_);
    navigation_.SetTextActivation(this, [](void* context, lv_obj_t* object) {
        auto* self = static_cast<Gamekit*>(context);
        if (self->ui_) (void)pxsys_reference_lvgl_key_activate(self->ui_, object);
    });
    lv_unlock();
    UnlockDisplay();
    return pointer_ && remote_ready && poll && status && navigation_ready;
}

bool Gamekit::InitializeTouch() {
    if (!touch_.Initialize() || !LockDisplay(1000)) return false;
    lv_lock();
    bool ready = true;
    for (uint8_t id = 0; id < 2; ++id) {
        auto* pointer = lv_indev_create();
        if (!pointer) { ready = false; break; }
        touch_pointers_[id] = pointer;
        lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(pointer, display_);
        lv_indev_set_driver_data(pointer, this);
        lv_indev_set_user_data(pointer, reinterpret_cast<void*>(uintptr_t(id)));
        lv_indev_set_read_cb(pointer, [](lv_indev_t* device, lv_indev_data_t* data) {
            auto* self = static_cast<Gamekit*>(lv_indev_get_driver_data(device));
            const auto id = uint8_t(reinterpret_cast<uintptr_t>(lv_indev_get_user_data(device)));
            bool more = false;
            const auto sample = self->touch_samples_.Read(id, more);
            if (sample.cancel) lv_indev_wait_release(device);
            data->point = {sample.x, sample.y};
            data->timestamp = lv_tick_get();
            data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->continue_reading = more;
        });
        lv_timer_set_period(lv_indev_get_read_timer(pointer), 5);
    }
    if (!ready) for (auto*& pointer : touch_pointers_) {
        if (pointer) lv_indev_delete(pointer);
        pointer = nullptr;
    }
    touch_success_tick_ = lv_tick_get();
    lv_unlock();
    UnlockDisplay();
    return ready;
}

void Gamekit::CancelTouch() {
    for (uint8_t id = 0; id < 2; ++id) {
        (void)touch_samples_.Cancel(id);
        if (touch_pointers_[id]) lv_indev_wait_release(touch_pointers_[id]);
    }
}

void Gamekit::PollTouch(uint32_t now) {
    if (!touch_ready_ || now - touch_tick_ < 10) return;
    touch_tick_ = now;
    TouchReport report;
    if (!touch_.Read(kTouchRotation, report)) {
        if (now - touch_success_tick_ >= 100 && touch_pressed_) {
            ESP_LOGW(kTag, "Touch report timeout; cancelling physical contacts");
            CancelTouch();
            touch_slots_ = {};
            touch_points_ = {};
            touch_pressed_ = false;
            touch_blocked_ = true;
        }
        return;
    }
    touch_success_tick_ = now;
    touch_pressed_ = report.count != 0;
    if (!touch_pressed_) touch_blocked_ = false;
    const auto points = touch_slots_.Update(report);
    for (uint8_t slot = 0; slot < 2; ++slot) {
        const auto& previous = touch_points_[slot];
        const auto& next = points[slot];
        if (previous.pressed && (!next.pressed || previous.id != next.id)) {
            if (!touch_blocked_) (void)touch_samples_.Push(slot, previous.x, previous.y, false);
            ESP_LOGI(kTag, "Touch UP slot=%u id=%u x=%u y=%u", slot, previous.id, previous.x, previous.y);
        }
        if (next.pressed) {
            if (!previous.pressed || previous.id != next.id)
                ESP_LOGI(kTag, "Touch DOWN slot=%u id=%u x=%u y=%u", slot, next.id, next.x, next.y);
            if (!touch_blocked_ && !touch_samples_.Push(slot, next.x, next.y, true)) {
                CancelTouch();
                touch_blocked_ = true;
            }
        }
    }
    touch_points_ = points;
}

void Gamekit::PollInput() {
    const bool screen_was_off = !screen_on_;
    const uint32_t now = lv_tick_get();
    const uint32_t elapsed = std::min(now - input_tick_, uint32_t(40));
    input_tick_ = now;
    PollTouch(now);
    uint32_t raw = 0;
    for (size_t i = 0; i < kButtonPins.size(); ++i)
        if (!gpio_get_level(gpio_num_t(kButtonPins[i]))) raw |= 1u << i;
    const uint32_t physical = raw | (touch_pressed_ || touch_samples_.active() ? 1u << 31 : 0);
    physical_active_.store(physical != 0);
    if (physical && !previous_raw_) {
        remote_controller_.store(0);
        portENTER_CRITICAL(&remote_lock_);
        remote_samples_.Clear();
        portEXIT_CRITICAL(&remote_lock_);
        for (auto* pointer : remote_pointers_) if (pointer) lv_indev_wait_release(pointer);
        if (takeover_) takeover_(takeover_context_);
    }
    previous_raw_ = physical;
    if (touch_pressed_) {
        touch_mode_ = true;
        force_pointer_ = false;
        navigation_.Cancel();
    } else if (raw) touch_mode_ = false;
    if (wake_requested_.exchange(false)) {
        screen_on_ = true;
        ApplyBacklight();
        lv_display_trigger_activity(display_);
    }
    pxadb::TestControlKey remote_key;
    while (xQueueReceive(remote_keys_, &remote_key, 0) == pdTRUE) {
        switch (remote_key) {
            case pxadb::TestControlKey::kBack:
                Back(); break;
            case pxadb::TestControlKey::kHome:
                if (ui_) (void)pxsys_reference_lvgl_home(ui_);
                break;
            case pxadb::TestControlKey::kVolumeUp:
                SetVolume(uint8_t(std::min(int(audio_.volume()) + 5, 100))); break;
            case pxadb::TestControlKey::kVolumeDown:
                SetVolume(uint8_t(std::max(int(audio_.volume()) - 5, 0))); break;
        }
    }
    const uint32_t buttons = debounce_.Update(raw, now);
    const uint32_t remote = remote_controller_.load();
    const uint32_t gamepad_buttons = buttons |
        ((remote & (1u << 17)) ? remote & 4095u : 0);
    if (gamepad_buttons && !touch_pressed_) touch_mode_ = false;
    if (screen_was_off && (gamepad_buttons || touch_pressed_)) {
        screen_on_ = true;
        ApplyBacklight();
        swallow_until_release_ = true;
        touch_blocked_ = touch_pressed_;
        CancelTouch();
        lv_display_trigger_activity(display_);
    }
    gamepad_consumed_ = !swallow_until_release_ &&
        pxsys_reference_lvgl_handle_gamepad(ui_, gamepad_buttons);
    if (swallow_until_release_ || gamepad_consumed_) {
        // A finger joining the wake-up key sequence must also be consumed.
        if ((swallow_until_release_ || gamepad_buttons) &&
            (touch_pressed_ || touch_samples_.active())) {
            touch_blocked_ = touch_pressed_;
            CancelTouch();
        }
        if (gamepad_buttons == 0 && !touch_pressed_) {
            swallow_until_release_ = false;
            shortcuts_ = {};
        }
        pointer_down_ = false;
        navigation_.Cancel();
        if (gamepad_consumed_ || screen_was_off || gamepad_buttons) {
            navigation_.Update(nullptr, false, 0, now);
            lv_obj_set_hidden(cursor_, true);
            shortcuts_ = {};
        }
        previous_delivered_ = 0;
        ReportController(0, now);
        return;
    }
    if (gamepad_buttons) lv_display_trigger_activity(display_);
    const bool system_shortcuts = force_pointer_ || NativeNavigation(false,
        NavigationScope() != nullptr, pxa_host_accepts_controller_input(),
        pxa_esp_surface_has_visible_surface());
    const auto action = shortcuts_.Update(gamepad_buttons, now, system_shortcuts);
    if (action.back) {
        navigation_.Cancel();
        Back();
    }
    if (action.recents && !action.home && ui_) {
        navigation_.Cancel();
        (void)pxsys_reference_lvgl_show_recents(ui_);
    }
    if (action.home && ui_) {
        force_pointer_ = false;
        (void)pxsys_reference_lvgl_home(ui_);
    }
    if (action.toggle_pointer) force_pointer_ = !force_pointer_;
    if (action.volume_delta)
        SetVolume(uint8_t(std::clamp(int(audio_.volume()) + action.volume_delta, 0, 100)));
    if (action.provision_wifi) ProvisionWifi();
    if (action.back || action.recents || action.home) {
        // A system key can change pages while A/B/START is still held. Drop
        // the old gesture and wait for release before delivering to the new UI.
        pointer_down_ = false;
        navigation_.Cancel();
        lv_indev_reset(pointer_, nullptr);
        previous_delivered_ = 0;
        swallow_until_release_ = gamepad_buttons != 0;
        touch_blocked_ = touch_pressed_;
        CancelTouch();
    }
    auto* scope = NavigationScope();
    const bool cursor_mode = force_pointer_;
    const bool focus_mode = doit_gamekit_1::NativeNavigation(cursor_mode,
        scope != nullptr, pxa_host_accepts_controller_input(),
        pxa_esp_surface_has_visible_surface());
    uint32_t delivered = swallow_until_release_ ? 0 : action.buttons;
    if (cursor_mode != pointer_mode_ || focus_mode != focus_mode_) {
        pointer_mode_ = cursor_mode;
        focus_mode_ = focus_mode;
        pointer_down_ = false;
        // Never turn an already-held game A into a click after a UI transition.
        swallow_until_release_ = gamepad_buttons != 0;
        delivered = 0;
        lv_indev_reset(pointer_, nullptr);
        navigation_.Cancel();
    }
    lv_obj_set_hidden(cursor_, !cursor_mode);
    if ((cursor_mode || focus_mode) && (delivered & B) && !(previous_delivered_ & B)) {
        navigation_.Cancel();
        Back();
        scope = NavigationScope();
    }
    // Act on a standalone START release so joining SELECT can reserve Home
    // without first opening Recents during the same button sequence.
    if ((cursor_mode || focus_mode) && ui_ && !(delivered & Start) &&
        !(gamepad_buttons & Select) && (previous_delivered_ & Start)) {
        navigation_.Cancel();
        (void)pxsys_reference_lvgl_show_recents(ui_);
        scope = NavigationScope();
    }
    previous_delivered_ = delivered;
    if (cursor_mode && !swallow_until_release_) {
        const int step = std::max(1, int(elapsed) / 4);
        cursor_x_ = std::clamp(cursor_x_ + step * (bool(delivered & Right) - bool(delivered & Left)), 0, kWidth - 1);
        cursor_y_ = std::clamp(cursor_y_ + step * (bool(delivered & Down) - bool(delivered & Up)), 0, kHeight - 1);
        pointer_down_ = (delivered & A) != 0;
    } else {
        pointer_down_ = false;
    }
    navigation_.Update(scope, focus_mode && !touch_mode_, swallow_until_release_ ? 0 : delivered, now);
    ReportController(cursor_mode || focus_mode ? 0 : delivered, now);
    if (!backlight_started_ && frame_ready_.load(std::memory_order_acquire)) {
        backlight_started_ = true;
        ApplyBacklight();
    }
}

lv_obj_t* Gamekit::NavigationScope() const {
    navigation_.SetInputActive(!touch_mode_ && !force_pointer_);
    // Native modal chrome takes priority over the runtime-managed PXA IME.
    auto* scope = ui_ ? pxsys_reference_lvgl_key_navigation_scope(ui_) : nullptr;
    return navigation_.ResolveScope(scope ? scope :
        static_cast<lv_obj_t*>(pxa_host_input_method_navigation_root()));
}

void Gamekit::Back() {
    if (navigation_.ClosePopup()) return;
    auto* ime_root = pxa_host_input_method_navigation_root();
    if (ime_root && ime_root == NavigationScope()) {
        navigation_.Cancel();
        if (pxa_host_dismiss_input_method()) return;
    }
    if (ui_) (void)pxsys_reference_lvgl_back(ui_);
    else (void)pxa_host_runtime_back();
}

void Gamekit::ReportController(uint32_t report, uint32_t now) {
    const uint32_t remote = remote_controller_.load();
    bool connected = true;
    if (remote & (1u << 16)) {
        connected = (remote & (1u << 17)) != 0;
        report = remote & 4095u;
    }
    if (gamepad_consumed_ || swallow_until_release_ || pointer_mode_ || focus_mode_) report = 0;
    // Retry failed queue submissions and periodically refresh complete state,
    // including zero, so app startup/resume never inherits stale held keys.
    if (report != reported_buttons_ || connected != reported_connected_ ||
        now - report_tick_ >= 50) {
        if (pxa_host_post_controller_state(0, connected, report)) {
            reported_buttons_ = report;
            reported_connected_ = connected;
            report_tick_ = now;
        }
    }
}

bool Gamekit::ConfigureDiagnostics() {
    pxadb::TestControlAdapter adapter;
    adapter.physical_input_state = [](void* context, pxadb::TestControlInputState* state) {
        if (!state) return false;
        auto* self = static_cast<Gamekit*>(context);
        state->physical_buttons = 0;
        for (size_t i = 0; i < kButtonPins.size(); ++i)
            if (!gpio_get_level(gpio_num_t(kButtonPins[i]))) state->physical_buttons |= 1u << i;
        state->physical_active = self->physical_active_.load();
        return true;
    };
    adapter.context = this;
    adapter.width = kWidth; adapter.height = kHeight;
    adapter.max_pointers = kDebugPointers;
    adapter.route_pointer_down = [](void* context) {
        auto* self = static_cast<Gamekit*>(context);
        self->wake_requested_.store(true);
        return !self->physical_active_.load();
    };
    adapter.inject_pointer = [](void* context, uint8_t id, uint16_t x, uint16_t y, bool pressed) {
        auto* self = static_cast<Gamekit*>(context);
        if (id >= kDebugPointers || x >= kWidth || y >= kHeight || self->physical_active_.load()) return false;
        if (!self->remote_pointers_[id]) {
            if (!self->LockDisplay(1000)) return false;
            lv_lock();
            const bool ready = self->CreateRemotePointer(id);
            lv_unlock();self->UnlockDisplay();
            if (!ready) return false;
        }
        portENTER_CRITICAL(&self->remote_lock_);
        const bool accepted = !self->physical_active_.load() &&
            self->remote_samples_.Push(id, x, y, pressed);
        portEXIT_CRITICAL(&self->remote_lock_);
        return accepted;
    };
    adapter.cancel_pointer = [](void* context, uint8_t id) {
        auto* self = static_cast<Gamekit*>(context);
        portENTER_CRITICAL(&self->remote_lock_);
        const bool accepted = self->remote_samples_.Cancel(id);
        portEXIT_CRITICAL(&self->remote_lock_);
        return accepted;
    };
    adapter.route_key = [](void* context, pxadb::TestControlKey key) {
        auto* self = static_cast<Gamekit*>(context);
        return xQueueSend(self->remote_keys_, &key, 0) == pdTRUE;
    };
    adapter.inject_controller = [](void* context, uint8_t controller,
                                    bool connected, uint32_t buttons) {
        auto* self = static_cast<Gamekit*>(context);
        if (controller || buttons > 4095 || self->physical_active_.load()) return false;
        self->remote_controller_.store((1u << 16) |
            (connected ? (1u << 17) : 0) | buttons);
        self->wake_requested_.store(true);
        return true;
    };
    adapter.cancel_controller = [](void* context) {
        static_cast<Gamekit*>(context)->remote_controller_.store(0);
    };
    adapter.set_input_takeover_callback = [](void* context, pxadb::TestControlTakeoverCallback callback, void* argument) {
        auto* self = static_cast<Gamekit*>(context);
        if (!self->LockDisplay(1000)) return;
        lv_lock();
        self->takeover_ = callback; self->takeover_context_ = argument;
        lv_unlock(); self->UnlockDisplay();
    };
    adapter.capture_rgb565 = [](void* context, uint16_t* pixels, size_t count, bool after_present,
                                pxadb::TestControlCaptureInfo* info) {
        auto* self = static_cast<Gamekit*>(context);
        if (!info) return false;
        if (after_present) {
            if (!self->LockDisplay(1000)) return false;
            lv_lock();
            const uint32_t before = self->completed_frames_.load();
            self->refresh_.BeginDiagnosticInvalidation();
            lv_obj_invalidate(lv_display_get_screen_active(self->display_));
            self->refresh_.EndDiagnosticInvalidation();
            lv_unlock(); self->UnlockDisplay();
            const TickType_t started = xTaskGetTickCount();
            while (self->completed_frames_.load() == before) {
                if (xTaskGetTickCount() - started > pdMS_TO_TICKS(1000)) return false;
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        return self->Capture(pixels, count, info);
    };
    adapter.presentation_fps_x10 = [](void*) {
        return pxa_board_performance_refresh();
    };
    const auto result = pxadb::ConfigureTestControl(&adapter);
    return result == ESP_OK || result == ESP_ERR_NOT_SUPPORTED;
}

bool Gamekit::InitializeWifi() {
    wifi_enabled_.store(pxa_board_wifi_get_enabled());
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected && sntp_ready_) (void)esp_netif_sntp_start();
    });
    WifiManagerConfig config;
    config.ssid_prefix = "DOIT-GameKit-1";
    config.language = "zh-CN";
    if (!wifi.Initialize(config)) return false;
    esp_sntp_config_t time = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time.start = false;
    time.wait_for_sync = false;
    sntp_ready_ = esp_netif_sntp_init(&time) == ESP_OK;
    if (wifi_enabled_.load()) wifi.StartStation();
    ESP_LOGI(kTag, "Wi-Fi startup switch: %s", wifi_enabled_.load() ? "on" : "off (NVS)");
    return true;
}

bool Gamekit::SetWifi(bool enabled) {
    if (!wifi_ready_) return false;
    if (!pxa_board_wifi_set_enabled(enabled)) return false;
    wifi_enabled_.store(enabled);
    auto& wifi = WifiManager::GetInstance();
    if (enabled) wifi.StartStation();
    else { wifi.StopConfigAp(); wifi.StopStation(); }
    PublishStatus();
    return true;
}

void Gamekit::ProvisionWifi() {
    if (!wifi_ready_ || !pxa_board_wifi_set_enabled(true)) return;
    wifi_enabled_.store(true);
    WifiManager::GetInstance().StartConfigAp();
    PublishStatus();
}

void Gamekit::AttachSystem(pxsys_standard_system_t* system, pxsys_reference_lvgl_t* ui) {
    system_ = system;
    ui_ = ui;
    boot_screen_.Hide();
    if (ui_) {
        (void)pxsys_reference_lvgl_set_lock_changed_callback(ui_, this, [](void* context, bool locked) {
            auto* self = static_cast<Gamekit*>(context);
            pxa_esp_surface_set_display_unlocked(!locked);
            (void)pxa_host_set_display_interactive(!locked);
            self->pointer_down_ = false;
            // PollInput cancels the keypad after the current LVGL event has
            // returned; unlocking can itself originate from an Enter event.
            self->swallow_until_release_ = true;
            lv_indev_reset(self->pointer_, nullptr);
        });
    }
    PublishStatus();
}

void Gamekit::SetVolume(uint8_t value) {
    audio_.SetVolume(value);
    // Persist through the integration status observer before another hotkey
    // can restart/power off the board; the one-second poll is too late.
    PublishStatus();
    ESP_LOGI(kTag, "Volume applied: %u%%", unsigned(audio_.volume()));
}

void Gamekit::PublishStatus() {
    if (!system_) return;
    static bool reported = false;
    if (!reported) {
        reported = true;
        ESP_LOGI(kTag, "System attached: LCD flushes=%lu completed=%lu",
                 static_cast<unsigned long>(flush_count_),
                 static_cast<unsigned long>(completed_frames_.load()));
    }
    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    const auto battery = battery_.Get();
    status.battery_valid = battery.valid;
    status.battery_percent = battery.percent;
    status.volume_supported = audio_ready_;
    status.volume_percent = audio_.volume();
    status.brightness_supported = 1;
    status.brightness_percent = backlight_.percent();
    auto& wifi = WifiManager::GetInstance();
    const bool connected = wifi_ready_ && wifi.IsConnected();
    status.wifi_supported = wifi_ready_;
    status.wifi_enabled = wifi_ready_ && wifi_enabled_.load();
    status.wifi_connected = status.network_connected = connected;
    status.network_type = connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    const int rssi = connected ? wifi.GetRssi() : -100;
    status.wifi_signal_level = status.network_signal_level =
        !connected ? 0 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
    (void)pxa_system_clock_update_status(&status);
    (void)pxsys_system_status_service_update(pxsys_standard_system_status(system_), &status);
}

Gamekit g_board;
const pxa_board_port_t kPort = {
    .struct_size = sizeof(pxa_board_port_t),
    .initialize = [](void*) { return g_board.Initialize(); },
    .display = [](void*) { return g_board.display(); },
    .display_profile = [](void*, pxsys_display_profile_t* output) {
        if (!output) return;
        pxsys_display_profile_init(output, kWidth, kHeight);
        output->density_dpi = CONFIG_PXA_DOIT1_DENSITY_DPI;
        output->shape = PXSYS_DISPLAY_SHAPE_RECTANGLE;
        output->safe_insets = {0, 0, 0, 0};
    },
    .set_network_enabled = [](void*, pxsys_network_type_t type, uint8_t enabled) {
        return type == PXSYS_NETWORK_WIFI && g_board.SetWifi(enabled)
            ? PXSYS_STATUS_OK : PXSYS_STATUS_UNSUPPORTED;
    },
    .set_level = [](void*, pxsys_level_control_t control, uint8_t percent) {
        if (control == PXSYS_LEVEL_CONTROL_VOLUME) g_board.SetVolume(percent);
        else if (control == PXSYS_LEVEL_CONTROL_BRIGHTNESS) g_board.SetBrightness(percent);
        else return PXSYS_STATUS_UNSUPPORTED;
        return PXSYS_STATUS_OK;
    },
    .performance_get = [](void*, pxsys_reference_performance_option_t option) {
        return pxa_board_performance_get(option);
    },
    .performance_set = [](void*, pxsys_reference_performance_option_t option, bool enabled) {
        return pxa_board_performance_set(option, enabled);
    },
    .system_ready = [](void*, pxsys_standard_system_t* system, pxsys_reference_lvgl_t* ui) {
        g_board.AttachSystem(system, ui);
    },
    .show_initial_frame = [](void*) { g_board.ShowInitialFrame(); },
    .configure_diagnostics = [](void*) { return g_board.ConfigureDiagnostics(); },
    .capture_displayed_rgb565 = [](void*, uint16_t* pixels, size_t count) { return g_board.Capture(pixels, count); },
    .set_idle_dim = [](void*, bool enabled, uint8_t percent) { g_board.SetIdleDim(enabled, percent); },
    .idle_screen_off = [](void*) { g_board.ScreenOff(); },
    .lock_display = [](void*, uint32_t timeout_ms) { return g_board.LockDisplay(timeout_ms); },
    .unlock_display = [](void*) { g_board.UnlockDisplay(); },
};
}

extern "C" bool pxa_board_register_selected(void) { return pxa_board_register(&kPort); }
