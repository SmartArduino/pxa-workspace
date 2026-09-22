#include "esp32s31_korvo_1_hardware.h"

#include "esp32s31_korvo_1_config.h"
#include "esp32s31_korvo_1_adc_calibration.h"
#include "esp32s31_korvo_1_pxa_surface.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <string>

#include <driver/i2c_master.h>
#include <driver/ppa.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <pxa/pxa_host.h>
#include <pxa_board_api.h>
#include <pxsys/standard_system.h>
#include <pxsys/reference_lvgl.h>
#include <wifi_manager.h>

namespace {
constexpr char kTag[] = "korvo_hw";
constexpr int kTouchPollMs = 6;
constexpr int64_t kTouchReleaseTimeoutUs = 150 * 1000;
constexpr uint16_t kGt1151ReadXyRegister = 0x814E;
constexpr uint8_t kGt1151StatusDataReady = 0x80;
constexpr uint8_t kGt1151MaxHardwarePoints = 10;
constexpr size_t kGt1151RecordBytes = 8;
constexpr size_t kGt1151ReportOverheadBytes = 3;
constexpr size_t kFrameBufferBytes =
    KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT * sizeof(uint16_t);
constexpr std::array<gpio_num_t, 16> kRgbDataPins = {
    KORVO_LCD_D0, KORVO_LCD_D1, KORVO_LCD_D2, KORVO_LCD_D3,
    KORVO_LCD_D4, KORVO_LCD_D5, KORVO_LCD_D6, KORVO_LCD_D7,
    KORVO_LCD_D8, KORVO_LCD_D9, KORVO_LCD_D10, KORVO_LCD_D11,
    KORVO_LCD_D12, KORVO_LCD_D13, KORVO_LCD_D14, KORVO_LCD_D15,
};

uint8_t SignalLevel(int rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

enum KorvoButton {
    kVolumeUp,
    kVolumeDown,
    kMode,
    kSet,
    kButtonCount,
};

struct KorvoAdcButtonDriver {
    button_driver_t base;
    adc_oneshot_unit_handle_t adc;
    uint16_t min_mv;
    uint16_t max_mv;
};

KorvoAdcButtonDriver kAdcButtonDrivers[kButtonCount];

uint8_t ReadKorvoAdcButton(button_driver_t* driver) {
    auto* button = reinterpret_cast<KorvoAdcButtonDriver*>(driver);
    uint32_t raw_sum = 0;
    int raw = 0;
    for (int sample = 0; sample < 4; ++sample) {
        if (adc_oneshot_read(button->adc, ADC_CHANNEL_0, &raw) != ESP_OK)
            return BUTTON_INACTIVE;
        raw_sum += static_cast<uint32_t>(raw);
    }
    int millivolts = 0;
    if (esp32s31_korvo_1_adc_raw_to_mv(
            ADC_UNIT_1, static_cast<int>(raw_sum / 4), &millivolts) != ESP_OK)
        return BUTTON_INACTIVE;
    return millivolts >= button->min_mv && millivolts <= button->max_mv
               ? BUTTON_ACTIVE
               : BUTTON_INACTIVE;
}
}

bool Esp32S31Korvo1Hardware::Initialize() {
    if (!InitializeI2c() || !InitializeDisplay()) return false;
    if (!InitializeTouch()) ESP_LOGW(kTag, "Continuing without GT1151 touch");
    if (!InitializeButtons()) ESP_LOGW(kTag, "Continuing without ADC buttons");
    audio_initialized_.store(audio_.Initialize(i2c_bus_));
    if (!audio_initialized_.load()) ESP_LOGW(kTag, "Continuing without ES8389 audio");
    if (!InitializeWifi()) ESP_LOGW(kTag, "Continuing without Wi-Fi");

    const esp_timer_create_args_t timer_config = {
        .callback = StatusTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "korvo_status",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_config, &status_timer_) != ESP_OK ||
        esp_timer_start_periodic(status_timer_, 1000 * 1000) != ESP_OK) {
        ESP_LOGW(kTag, "System status timer is unavailable");
    }
    return true;
}

void Esp32S31Korvo1Hardware::AttachSystem(
    pxsys_standard_system_t* system, pxsys_reference_lvgl_t* reference_ui) {
    system_ = system;
    reference_ui_ = reference_ui;
    PublishStatus();
}

void Esp32S31Korvo1Hardware::SetWifiEnabled(bool enabled) {
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

void Esp32S31Korvo1Hardware::SetVolume(uint8_t percent) {
    audio_.SetVolume(percent);
    ScheduleStatusUpdate();
}

bool Esp32S31Korvo1Hardware::InitializeWifi() {
    WifiManagerConfig config;
    config.ssid_prefix = "S31-Korvo-1";
    config.language = "zh-CN";
    auto& wifi = WifiManager::GetInstance();
    wifi.SetEventCallback([this](WifiEvent event, const std::string&) {
        if (event == WifiEvent::Connected || event == WifiEvent::Disconnected ||
            event == WifiEvent::ConfigModeEnter || event == WifiEvent::ConfigModeExit) {
            ScheduleStatusUpdate();
        }
    });
    if (!wifi.Initialize(config)) return false;
    wifi_initialized_.store(true);
    wifi.StartStation();
    return true;
}

bool Esp32S31Korvo1Hardware::InitializeButtons() {
    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&unit_config, &button_adc_) != ESP_OK) return false;
    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(button_adc_, ADC_CHANNEL_0, &channel_config) != ESP_OK ||
        esp32s31_korvo_1_adc_calibration_init(
            button_adc_, ADC_UNIT_1, ADC_CHANNEL_0) != ESP_OK)
        return false;

    constexpr uint16_t kCentersMv[kButtonCount] = {380, 820, 1340, 1870};
    constexpr uint16_t kRangesMv[kButtonCount][2] = {
        {160, 600}, {600, 1080}, {1080, 1605}, {1605, 1935},
    };
    const button_config_t button_config = {
        .long_press_time = 1200,
        .short_press_time = 0,
    };
    for (size_t index = 0; index < kButtonCount; ++index) {
        auto& driver = kAdcButtonDrivers[index];
        driver.base.get_key_level = ReadKorvoAdcButton;
        driver.adc = button_adc_;
        driver.min_mv = kRangesMv[index][0];
        driver.max_mv = kRangesMv[index][1];
        if (iot_button_create(&button_config, &driver.base, &buttons_[index]) != ESP_OK)
            return false;
        ESP_LOGI(kTag, "ADC button %u centered at %u mV", static_cast<unsigned>(index),
                 kCentersMv[index]);
    }

    iot_button_register_cb(buttons_[kVolumeUp], BUTTON_PRESS_DOWN, nullptr,
                           [](void*, void* context) {
                               auto* self = static_cast<Esp32S31Korvo1Hardware*>(context);
                               if (!pxa_host_captures_volume_keys() ||
                                   !pxa_host_post_key(PXA_HOST_KEY_VOLUME_UP))
                                   self->AdjustVolume(10);
                           }, this);
    iot_button_register_cb(buttons_[kVolumeUp], BUTTON_PRESS_UP, nullptr,
                           [](void*, void*) {
                               if (pxa_host_captures_volume_keys())
                                   (void)pxa_host_post_key(PXA_HOST_KEY_VOLUME_UP_RELEASED);
                           }, this);
    iot_button_register_cb(buttons_[kVolumeDown], BUTTON_PRESS_DOWN, nullptr,
                           [](void*, void* context) {
                               auto* self = static_cast<Esp32S31Korvo1Hardware*>(context);
                               if (!pxa_host_captures_volume_keys() ||
                                   !pxa_host_post_key(PXA_HOST_KEY_VOLUME_DOWN))
                                   self->AdjustVolume(-10);
                           }, this);
    iot_button_register_cb(buttons_[kVolumeDown], BUTTON_PRESS_UP, nullptr,
                           [](void*, void*) {
                               if (pxa_host_captures_volume_keys())
                                   (void)pxa_host_post_key(PXA_HOST_KEY_VOLUME_DOWN_RELEASED);
                           }, this);
    iot_button_register_cb(buttons_[kMode], BUTTON_SINGLE_CLICK, nullptr,
                           [](void*, void* context) {
                               static_cast<Esp32S31Korvo1Hardware*>(context)->NavigateHome();
                           }, this);
    iot_button_register_cb(buttons_[kSet], BUTTON_SINGLE_CLICK, nullptr,
                           [](void*, void* context) {
                               static_cast<Esp32S31Korvo1Hardware*>(context)->StartWifiProvisioning();
                           }, this);
    return true;
}

void Esp32S31Korvo1Hardware::AdjustVolume(int delta) {
    const int target = std::clamp(static_cast<int>(volume()) + delta, 0, 100);
    SetVolume(static_cast<uint8_t>(target));
}

void Esp32S31Korvo1Hardware::NavigateHome() {
    if (reference_ui_ == nullptr) return;
    lv_lock();
    lv_async_call([](void* context) {
        auto* self = static_cast<Esp32S31Korvo1Hardware*>(context);
        if (self->reference_ui_ != nullptr)
            (void)pxsys_reference_lvgl_home(self->reference_ui_);
    }, this);
    lv_unlock();
}

void Esp32S31Korvo1Hardware::StartWifiProvisioning() {
    if (!wifi_initialized_.load()) return;
    wifi_enabled_.store(true);
    WifiManager::GetInstance().StartConfigAp();
    ScheduleStatusUpdate();
}

bool Esp32S31Korvo1Hardware::InitializeI2c() {
    const i2c_master_bus_config_t config = {
        .i2c_port = KORVO_I2C_PORT,
        .sda_io_num = KORVO_I2C_SDA,
        .scl_io_num = KORVO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {.enable_internal_pullup = true},
    };
    return i2c_new_master_bus(&config, &i2c_bus_) == ESP_OK;
}

bool Esp32S31Korvo1Hardware::InitializeDisplay() {
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = KORVO_DISPLAY_PIXEL_CLOCK_HZ,
            .h_res = KORVO_DISPLAY_WIDTH,
            .v_res = KORVO_DISPLAY_HEIGHT,
            .hsync_pulse_width = 1,
            .hsync_back_porch = 40,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 1,
            .vsync_back_porch = 10,
            .vsync_front_porch = 5,
            .flags = {.pclk_active_neg = true},
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 3,
        .dma_burst_size = 64,
        .hsync_gpio_num = KORVO_LCD_HSYNC,
        .vsync_gpio_num = KORVO_LCD_VSYNC,
        .de_gpio_num = KORVO_LCD_DE,
        .pclk_gpio_num = KORVO_LCD_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        .flags = {.fb_in_psram = true},
    };
    for (gpio_num_t& pin : panel_config.data_gpio_nums) pin = GPIO_NUM_NC;
    for (size_t i = 0; i < kRgbDataPins.size(); ++i) panel_config.data_gpio_nums[i] = kRgbDataPins[i];
    if (esp_lcd_new_rgb_panel(&panel_config, &panel_) != ESP_OK ||
        esp_lcd_panel_reset(panel_) != ESP_OK || esp_lcd_panel_init(panel_) != ESP_OK) return false;

    if (esp_lcd_rgb_panel_get_frame_buffer(
            panel_, 3, &frame_buffers_[0], &frame_buffers_[1],
            &frame_buffers_[2]) != ESP_OK ||
        frame_buffers_[0] == nullptr || frame_buffers_[1] == nullptr ||
        frame_buffers_[2] == nullptr)
        return false;
    const ppa_client_config_t sync_ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&sync_ppa_config, &sync_ppa_client_) != ESP_OK) {
        sync_ppa_client_ = nullptr;
        ESP_LOGW(kTag, "PPA framebuffer sync unavailable; using CPU copies");
    }
    frame_done_sem_ =
        xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (frame_done_sem_ == nullptr) return false;

    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = OnFrameBufferComplete,
    };
    if (esp_lcd_rgb_panel_register_event_callbacks(panel_, &callbacks, this) != ESP_OK)
        return false;

    const lvgl_port_cfg_t port_config = {
        .task_priority = 4,
        .task_stack = 12288,
        .task_affinity = 1,
        .task_max_sleep_ms = 100,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = 2,
    };
    if (lvgl_port_init(&port_config) != ESP_OK) return false;
    constexpr size_t kDrawBufferBytes =
        KORVO_DISPLAY_WIDTH * KORVO_DISPLAY_HEIGHT * sizeof(uint16_t);
    display_ = lv_display_create(KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT);
    if (display_ == nullptr) return false;
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    const esp_err_t decoder_error = esp_lv_decoder_init(&image_decoder_);
    if (decoder_error != ESP_OK) {
        ESP_LOGE(kTag, "Failed to initialize image decoder: %s",
                 esp_err_to_name(decoder_error));
        return false;
    }
    ESP_LOGI(kTag, "esp_lv_decoder initialized");
    /* The RGB driver initially scans fb0. Start LVGL on fb1 and rotate through
     * fb2 before returning to fb0, so one frame can render while the previous
     * submitted frame waits for the panel to release its buffer. */
    if (lv_draw_buf_init(&draw_buffers_[0], KORVO_DISPLAY_WIDTH,
                         KORVO_DISPLAY_HEIGHT, LV_COLOR_FORMAT_RGB565,
                         LV_STRIDE_AUTO, frame_buffers_[1],
                         kDrawBufferBytes) != LV_RESULT_OK ||
        lv_draw_buf_init(&draw_buffers_[1], KORVO_DISPLAY_WIDTH,
                         KORVO_DISPLAY_HEIGHT, LV_COLOR_FORMAT_RGB565,
                         LV_STRIDE_AUTO, frame_buffers_[2],
                         kDrawBufferBytes) != LV_RESULT_OK ||
        lv_draw_buf_init(&draw_buffers_[2], KORVO_DISPLAY_WIDTH,
                         KORVO_DISPLAY_HEIGHT, LV_COLOR_FORMAT_RGB565,
                         LV_STRIDE_AUTO, frame_buffers_[0],
                         kDrawBufferBytes) != LV_RESULT_OK)
        return false;
    lv_display_set_draw_buffers(display_, &draw_buffers_[0],
                                &draw_buffers_[1]);
    lv_display_set_3rd_draw_buffer(display_, &draw_buffers_[2]);
    lv_display_set_render_mode(display_, LV_DISPLAY_RENDER_MODE_DIRECT);
    latest_frame_buffer_ = frame_buffers_[0];
    /* Give LVGL enough candidate tiles to keep both software draw units busy
     * when the full-width settings view is invalidated. */
    lv_display_set_tile_cnt(display_, 4);
    lv_display_set_user_data(display_, this);
    lv_display_set_flush_cb(display_, FlushDisplay);
    lv_display_set_sync_cb(display_, SyncDisplay);
    lv_display_add_event_cb(display_, OnDisplayEvent, LV_EVENT_ALL, this);
    if (!korvo_pxa_surface::Install(display_, panel_, frame_done_sem_,
                                    OnDirectScanoutTransition, this)) {
        ESP_LOGW(kTag, "Continuing without PXA Surface presentation");
    }
    return display_ != nullptr;
}

void Esp32S31Korvo1Hardware::RecordRenderTime(uint32_t duration_us) {
    ++perf_render_count_;
    perf_render_total_us_ += duration_us;
    perf_render_max_us_ = std::max(perf_render_max_us_, duration_us);
}

void Esp32S31Korvo1Hardware::RecordDrawTime(uint32_t duration_us) {
    ++perf_draw_count_;
    perf_draw_total_us_ += duration_us;
    perf_draw_max_us_ = std::max(perf_draw_max_us_, duration_us);
}

void Esp32S31Korvo1Hardware::RecordPresentTime(uint32_t flush_us,
                                                uint32_t vsync_us,
                                                bool vsync_ready) {
    ++perf_flushes_;
    perf_flush_total_us_ += flush_us;
    perf_vsync_total_us_ += vsync_us;
    perf_flush_max_us_ = std::max(perf_flush_max_us_, flush_us);
    perf_vsync_max_us_ = std::max(perf_vsync_max_us_, vsync_us);
    if (!vsync_ready) ++perf_vsync_timeouts_;
}

void Esp32S31Korvo1Hardware::RecordFrame() {
    const int64_t now_us = esp_timer_get_time();
    if (perf_report_started_us_ == 0) perf_report_started_us_ = now_us;
    ++perf_frames_;
    if (now_us - perf_report_started_us_ < 1000 * 1000) return;

    const uint32_t render_average_us = perf_render_count_ == 0 ? 0 :
        static_cast<uint32_t>(perf_render_total_us_ / perf_render_count_);
    const uint32_t draw_average_us = perf_draw_count_ == 0 ? 0 :
        static_cast<uint32_t>(perf_draw_total_us_ / perf_draw_count_);
    const uint32_t flush_average_us = perf_flushes_ == 0 ? 0 :
        static_cast<uint32_t>(perf_flush_total_us_ / perf_flushes_);
    const uint32_t vsync_average_us = perf_flushes_ == 0 ? 0 :
        static_cast<uint32_t>(perf_vsync_total_us_ / perf_flushes_);
    const uint32_t sync_count = perf_sync_ppa_count_ + perf_sync_cpu_count_;
    const uint32_t sync_average_us = sync_count == 0 ? 0 :
        static_cast<uint32_t>(perf_sync_total_us_ / sync_count);
    ESP_LOGI(kTag,
             "PERF frame=%u flush=%u render=%u/%u ms draw=%u/%u ms sync=ppa%u/cpu%u %u/%u ms fb=%u/%u ms vsync=%u/%u ms timeout=%u",
             perf_frames_, static_cast<uint32_t>(perf_flushes_),
             render_average_us / 1000, perf_render_max_us_ / 1000,
             draw_average_us / 1000, perf_draw_max_us_ / 1000,
             perf_sync_ppa_count_, perf_sync_cpu_count_,
             sync_average_us / 1000, perf_sync_max_us_ / 1000,
             flush_average_us / 1000, perf_flush_max_us_ / 1000,
             vsync_average_us / 1000, perf_vsync_max_us_ / 1000,
             perf_vsync_timeouts_);
    perf_report_started_us_ = now_us;
    perf_frames_ = 0;
    perf_flushes_ = 0;
    perf_render_count_ = 0;
    perf_draw_count_ = 0;
    perf_sync_ppa_count_ = 0;
    perf_sync_cpu_count_ = 0;
    perf_vsync_timeouts_ = 0;
    perf_render_total_us_ = 0;
    perf_draw_total_us_ = 0;
    perf_sync_total_us_ = 0;
    perf_flush_total_us_ = 0;
    perf_vsync_total_us_ = 0;
    perf_render_max_us_ = 0;
    perf_draw_max_us_ = 0;
    perf_sync_max_us_ = 0;
    perf_flush_max_us_ = 0;
    perf_vsync_max_us_ = 0;
}

void Esp32S31Korvo1Hardware::SyncFrameBuffers(lv_display_t* display,
                                               const lv_area_t* area) {
    if (display == nullptr || area == nullptr) return;
    lv_draw_buf_t* const draw_buffer = lv_display_get_buf_active(display);
    void* const destination = draw_buffer != nullptr ? draw_buffer->data : nullptr;
    void* const source = latest_frame_buffer_;
    if (source == nullptr || destination == nullptr) {
        ESP_LOGE(kTag, "Cannot resolve RGB framebuffer sync buffers");
        lv_display_sync_ready(display);
        return;
    }
    RecordDeferredSyncArea(area);
    if (source == destination) {
        lv_display_sync_ready(display);
        return;
    }

    CopyFrameBufferArea(source, destination, area);
    lv_display_sync_ready(display);
}

void Esp32S31Korvo1Hardware::RecordDeferredSyncArea(
    const lv_area_t* area) {
    if (area == nullptr) return;
    for (size_t index = 0; index < deferred_sync_area_count_; ++index) {
        lv_area_t& recorded = deferred_sync_areas_[index];
        const bool separated =
            area->x2 < recorded.x1 || area->x1 > recorded.x2 ||
            area->y2 < recorded.y1 || area->y1 > recorded.y2;
        if (!separated) {
            recorded.x1 = std::min(recorded.x1, area->x1);
            recorded.y1 = std::min(recorded.y1, area->y1);
            recorded.x2 = std::max(recorded.x2, area->x2);
            recorded.y2 = std::max(recorded.y2, area->y2);
            return;
        }
    }
    if (deferred_sync_area_count_ < kDeferredSyncAreaCapacity) {
        deferred_sync_areas_[deferred_sync_area_count_++] = *area;
        return;
    }

    // Preserve correctness if LVGL produces more regions than its normal
    // invalidation capacity by folding overflow into the last rectangle.
    lv_area_t& recorded =
        deferred_sync_areas_[kDeferredSyncAreaCapacity - 1];
    recorded.x1 = std::min(recorded.x1, area->x1);
    recorded.y1 = std::min(recorded.y1, area->y1);
    recorded.x2 = std::max(recorded.x2, area->x2);
    recorded.y2 = std::max(recorded.y2, area->y2);
}

void Esp32S31Korvo1Hardware::SyncReleasedFrameBuffer(
    void* next_frame_buffer) {
    if (deferred_sync_area_count_ == 0 || latest_frame_buffer_ == nullptr ||
        next_frame_buffer == nullptr)
        return;

    void* released_buffer = nullptr;
    for (void* frame_buffer : frame_buffers_) {
        if (frame_buffer != latest_frame_buffer_ &&
            frame_buffer != next_frame_buffer) {
            released_buffer = frame_buffer;
            break;
        }
    }
    if (released_buffer == nullptr) {
        ESP_LOGE(kTag, "Cannot resolve released RGB framebuffer");
        return;
    }

    for (size_t index = 0; index < deferred_sync_area_count_; ++index) {
        CopyFrameBufferArea(latest_frame_buffer_, released_buffer,
                            &deferred_sync_areas_[index]);
    }
    deferred_sync_area_count_ = 0;
}

void Esp32S31Korvo1Hardware::CopyFrameBufferArea(
    void* source_buffer, void* destination_buffer, const lv_area_t* area) {
    if (source_buffer == nullptr || destination_buffer == nullptr ||
        source_buffer == destination_buffer || area == nullptr)
        return;

    const int64_t started_us = esp_timer_get_time();
    const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
    const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
    bool copied_by_ppa = false;
    if (sync_ppa_client_ != nullptr && width != 0 && height != 0) {
        const ppa_srm_oper_config_t config = {
            .in = {.buffer = source_buffer,
                   .pic_w = KORVO_DISPLAY_WIDTH,
                   .pic_h = KORVO_DISPLAY_HEIGHT,
                   .block_w = width,
                   .block_h = height,
                   .block_offset_x = static_cast<uint32_t>(area->x1),
                   .block_offset_y = static_cast<uint32_t>(area->y1),
                   .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .out = {.buffer = destination_buffer,
                    .buffer_size = kFrameBufferBytes,
                    .pic_w = KORVO_DISPLAY_WIDTH,
                    .pic_h = KORVO_DISPLAY_HEIGHT,
                    .block_offset_x = static_cast<uint32_t>(area->x1),
                    .block_offset_y = static_cast<uint32_t>(area->y1),
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 1.0f,
            .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        copied_by_ppa =
            ppa_do_scale_rotate_mirror(sync_ppa_client_, &config) == ESP_OK;
    }
    if (!copied_by_ppa) {
        auto* const source = static_cast<uint8_t*>(source_buffer);
        auto* const destination = static_cast<uint8_t*>(destination_buffer);
        const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint16_t);
        for (uint32_t row = 0; row < height; ++row) {
            const size_t offset =
                (static_cast<size_t>(area->y1) + row) * KORVO_DISPLAY_WIDTH +
                static_cast<size_t>(area->x1);
            std::memcpy(destination + offset * sizeof(uint16_t),
                        source + offset * sizeof(uint16_t), row_bytes);
        }
    }
    if (copied_by_ppa) ++perf_sync_ppa_count_;
    else ++perf_sync_cpu_count_;
    const uint32_t duration_us =
        static_cast<uint32_t>(esp_timer_get_time() - started_us);
    perf_sync_total_us_ += duration_us;
    perf_sync_max_us_ = std::max(perf_sync_max_us_, duration_us);
}

void Esp32S31Korvo1Hardware::FlushDisplay(lv_display_t* display,
                                           const lv_area_t* area,
                                           uint8_t* pixels) {
    (void)area;
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(
        lv_display_get_user_data(display));
    /* While the presenter scans a full-screen Surface out directly, LVGL's
     * refresh timer is paused and must not touch the panel frame buffers. */
    if (korvo_pxa_surface::DirectScanoutActive()) {
        lv_display_flush_ready(display);
        return;
    }

    if (hardware != nullptr && hardware->render_started_us_ != 0 &&
        !hardware->draw_measured_) {
        hardware->draw_measured_ = true;
        hardware->RecordDrawTime(static_cast<uint32_t>(
            esp_timer_get_time() - hardware->render_started_us_));
    }

    if (hardware != nullptr && lv_display_flush_is_last(display)) {
        /* A Surface frame is composed once after LVGL finishes all dirty
         * regions. Surface-only updates deliberately invalidate one pixel,
         * avoiding an otherwise redundant full-screen LVGL redraw. */
        korvo_pxa_surface::ComposeFrame(pixels);
        pxa_board_performance_note_frame();
        pxa_board_performance_draw_rgb565(
            reinterpret_cast<uint16_t*>(pixels), KORVO_DISPLAY_WIDTH,
            KORVO_DISPLAY_HEIGHT, KORVO_DISPLAY_WIDTH, 0, 0,
            KORVO_DISPLAY_WIDTH, KORVO_DISPLAY_HEIGHT, false);
        const int64_t wait_started_us = esp_timer_get_time();
        const bool frame_ready = hardware->WaitForPendingFrame();
        const int64_t flush_started_us = esp_timer_get_time();
        if (!frame_ready) {
            ESP_LOGW(kTag, "Previous RGB frame did not complete");
            hardware->RecordPresentTime(
                0, static_cast<uint32_t>(flush_started_us - wait_started_us),
                false);
            lv_display_flush_ready(display);
            return;
        }
        /* The previous flip has completed, so the third buffer is no longer
         * scanned by the panel. Propagate the prior frame's dirty regions to
         * it before this frame is queued. This keeps all three direct-mode
         * buffers coherent without writing into the live scanout buffer. */
        hardware->SyncReleasedFrameBuffer(pixels);
        /* pixels is one of the panel frame buffers, so this only writes back
         * the CPU cache and queues the buffer for the next VSYNC; the RGB
         * driver switches buffers without copying any pixels. */
        (void)xSemaphoreTake(hardware->frame_done_sem_, 0);
        const bool submitted =
            esp_lcd_panel_draw_bitmap(hardware->panel_, 0, 0,
                                      KORVO_DISPLAY_WIDTH,
                                      KORVO_DISPLAY_HEIGHT, pixels) == ESP_OK;
        if (!submitted) {
            ESP_LOGW(kTag, "RGB frame buffer switch failed");
        } else {
            hardware->frame_switch_pending_ = true;
            hardware->latest_frame_buffer_ = pixels;
        }
        const int64_t flush_finished_us = esp_timer_get_time();
        hardware->RecordPresentTime(
            static_cast<uint32_t>(flush_finished_us - flush_started_us),
            static_cast<uint32_t>(flush_started_us - wait_started_us),
            frame_ready && submitted);
    }
    lv_display_flush_ready(display);
}

void Esp32S31Korvo1Hardware::SyncDisplay(lv_display_t* display,
                                         const lv_area_t* area) {
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(
        lv_display_get_user_data(display));
    if (hardware != nullptr) hardware->SyncFrameBuffers(display, area);
    else lv_display_sync_ready(display);
}

void Esp32S31Korvo1Hardware::OnDisplayEvent(lv_event_t* event) {
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(
        lv_event_get_user_data(event));
    if (hardware == nullptr) return;
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        hardware->render_started_us_ = esp_timer_get_time();
        hardware->draw_measured_ = false;
    } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY &&
               hardware->render_started_us_ != 0) {
        const int64_t finished_us = esp_timer_get_time();
        hardware->RecordRenderTime(static_cast<uint32_t>(
            finished_us - hardware->render_started_us_));
        hardware->render_started_us_ = 0;
        hardware->RecordFrame();
    }
}

bool Esp32S31Korvo1Hardware::WaitForPendingFrame() {
    if (!frame_switch_pending_) return true;
    if (frame_done_sem_ == nullptr ||
        xSemaphoreTake(frame_done_sem_, pdMS_TO_TICKS(100)) != pdTRUE)
        return false;
    frame_switch_pending_ = false;
    return true;
}

void Esp32S31Korvo1Hardware::AlignAfterDirectScanout(
    void* next_buffer, void* displayed_buffer) {
    if (display_ == nullptr || next_buffer == nullptr ||
        displayed_buffer == nullptr)
        return;

    lv_draw_buf_t* const active = lv_display_get_buf_active(display_);
    lv_draw_buf_t* next = nullptr;
    for (auto& draw_buffer : draw_buffers_) {
        if (draw_buffer.data == next_buffer) {
            next = &draw_buffer;
            break;
        }
    }
    if (active == nullptr || next == nullptr) {
        ESP_LOGE(kTag, "Cannot align LVGL after direct scanout");
        return;
    }
    if (active != next) {
        std::swap(active->data, next->data);
        std::swap(active->unaligned_data, next->unaligned_data);
    }
    latest_frame_buffer_ = displayed_buffer;
    frame_switch_pending_ = false;
    deferred_sync_area_count_ = 0;
    (void)xSemaphoreTake(frame_done_sem_, 0);
}

bool Esp32S31Korvo1Hardware::OnDirectScanoutTransition(
    bool entering, void* next_buffer, void* displayed_buffer, void* context) {
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(context);
    if (hardware == nullptr) return false;
    if (entering) return hardware->WaitForPendingFrame();
    hardware->AlignAfterDirectScanout(next_buffer, displayed_buffer);
    return true;
}

bool Esp32S31Korvo1Hardware::OnFrameBufferComplete(
    esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*,
    void* context) {
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(context);
    if (hardware == nullptr || hardware->frame_done_sem_ == nullptr)
        return false;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(hardware->frame_done_sem_, &task_woken);
    return task_woken == pdTRUE;
}

bool Esp32S31Korvo1Hardware::InitializeTouch() {
    const esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS,
        .scl_speed_hz = 100000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .lcd_param_bits = 0,
        .flags = {.disable_control_phase = true},
        .transaction_timeout_ms = 100,
    };
    if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io_) != ESP_OK) return false;
    const esp_lcd_touch_config_t touch_config = {
        .x_max = KORVO_DISPLAY_WIDTH,
        .y_max = KORVO_DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
    };
    if (esp_lcd_touch_new_i2c_gt1151(touch_io_, &touch_config, &touch_) != ESP_OK) return false;

    // esp_lvgl_port publishes only the first point from a multi-touch report.
    // Give every GT1151 contact a dedicated pointer indev instead, so the PXA
    // bridge emits one stable pointer_id per finger.
    if (!lvgl_port_lock(100)) return false;
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
        touch_indevs_[i] = indev;
    }
    lvgl_port_unlock();
    if (touch_indevs_[0] == nullptr) return false;
    ESP_LOGI(kTag, "GT1151 ready: %d independent touch pointers",
             kTouchMaxPointers);
    return true;
}

void Esp32S31Korvo1Hardware::TouchReadCallback(lv_indev_t* indev,
                                                lv_indev_data_t* data) {
    auto* hardware = static_cast<Esp32S31Korvo1Hardware*>(
        lv_indev_get_driver_data(indev));
    if (hardware == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    const int slot = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_indev_get_user_data(indev)));
    if (slot < 0 || slot >= kTouchMaxPointers) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // All indevs share one controller report. The first callback in each
    // polling window refreshes that snapshot; the remaining callbacks merely
    // publish their own slot and never consume another report.
    const int64_t now_us = esp_timer_get_time();
    if (now_us - hardware->touch_last_poll_us_ >= kTouchPollMs * 1000) {
        hardware->PollTouchController();
    }
    portENTER_CRITICAL(&hardware->touch_lock_);
    data->point = hardware->touch_slot_point_[slot];
    data->state = hardware->touch_slot_pressed_[slot]
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&hardware->touch_lock_);
}

void Esp32S31Korvo1Hardware::PollTouchController() {
    uint8_t status = 0;
    const int64_t now_us = esp_timer_get_time();
    touch_last_poll_us_ = now_us;
    const auto expire_stale_contacts = [this, now_us] {
        portENTER_CRITICAL(&touch_lock_);
        if (touch_last_report_us_ != 0 &&
            now_us - touch_last_report_us_ >= kTouchReleaseTimeoutUs) {
            for (int slot = 0; slot < kTouchMaxPointers; ++slot) {
                touch_slot_pressed_[slot] = false;
            }
        }
        portEXIT_CRITICAL(&touch_lock_);
    };
    if (touch_io_ == nullptr ||
        esp_lcd_panel_io_rx_param(touch_io_, kGt1151ReadXyRegister, &status,
                                  sizeof(status)) != ESP_OK) {
        expire_stale_contacts();
        return;
    }

    // A clear data-ready bit means the controller has no new report. Its low
    // nibble is then zero even while a finger remains down, so retaining the
    // previous snapshot is essential for a continuous press.
    if ((status & kGt1151StatusDataReady) == 0) return;

    const uint8_t reported_count = status & 0x0f;
    uint8_t clear = 0;
    if (reported_count == 0 || reported_count > kGt1151MaxHardwarePoints) {
        (void)esp_lcd_panel_io_tx_param(touch_io_, kGt1151ReadXyRegister,
                                        &clear, sizeof(clear));
        portENTER_CRITICAL(&touch_lock_);
        for (int slot = 0; slot < kTouchMaxPointers; ++slot) {
            touch_slot_pressed_[slot] = false;
        }
        portEXIT_CRITICAL(&touch_lock_);
        return;
    }

    {
        constexpr size_t kMaxReportBytes =
            kGt1151ReportOverheadBytes +
            kGt1151RecordBytes * kGt1151MaxHardwarePoints;
        uint8_t report[kMaxReportBytes] = {};
        const size_t report_size = kGt1151ReportOverheadBytes +
                                   kGt1151RecordBytes * reported_count;
        if (esp_lcd_panel_io_rx_param(touch_io_, kGt1151ReadXyRegister,
                                      report, report_size) != ESP_OK) {
            expire_stale_contacts();
            return;
        }
        (void)esp_lcd_panel_io_tx_param(touch_io_, kGt1151ReadXyRegister,
                                        &clear, sizeof(clear));
        uint8_t checksum = 0;
        for (size_t i = 0; i < report_size; ++i) checksum += report[i];
        if (checksum != 0) {
            expire_stale_contacts();
            return;
        }

        bool slot_seen[kTouchMaxPointers] = {};
        portENTER_CRITICAL(&touch_lock_);
        for (uint8_t point = 0; point < reported_count; ++point) {
            const uint8_t* record = report + 1 + point * kGt1151RecordBytes;
            const uint8_t track_id = record[0] & 0x0f;
            int slot = -1;
            for (int candidate = 0; candidate < kTouchMaxPointers;
                 ++candidate) {
                if (touch_slot_pressed_[candidate] &&
                    !slot_seen[candidate] &&
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
            touch_slot_point_[slot].x =
                static_cast<int32_t>(record[1] | (record[2] << 8));
            touch_slot_point_[slot].y =
                static_cast<int32_t>(record[3] | (record[4] << 8));
        }
        for (int slot = 0; slot < kTouchMaxPointers; ++slot) {
            if (!slot_seen[slot]) touch_slot_pressed_[slot] = false;
        }
        touch_last_report_us_ = now_us;
        portEXIT_CRITICAL(&touch_lock_);
    }
}

void Esp32S31Korvo1Hardware::ScheduleStatusUpdate() {
    if (system_ == nullptr || status_update_pending_.exchange(true)) return;
    lv_lock();
    lv_async_call(StatusOnLvgl, this);
    lv_unlock();
}

void Esp32S31Korvo1Hardware::PublishStatus() {
    if (system_ == nullptr) return;

    pxsys_system_status_snapshot_t status;
    pxsys_system_status_snapshot_init(&status);
    const std::time_t now = std::time(nullptr);
    struct tm local = {};
    if (localtime_r(&now, &local) != nullptr && local.tm_year + 1900 >= 2020) {
        status.time_valid = 1;
        status.hour = local.tm_hour;
        status.minute = local.tm_min;
    }

    const bool wifi_ready = wifi_initialized_.load();
    const bool wifi_enabled = wifi_ready && wifi_enabled_.load();
    bool connected = false;
    int rssi = -100;
    if (wifi_enabled) {
        auto& wifi = WifiManager::GetInstance();
        connected = wifi.IsConnected();
        if (connected) rssi = wifi.GetRssi();
    }
    status.network_connected = connected;
    status.network_type = connected ? PXSYS_NETWORK_WIFI : PXSYS_NETWORK_NONE;
    status.network_signal_level = connected ? SignalLevel(rssi) : 0;
    status.wifi_supported = wifi_ready;
    status.wifi_enabled = wifi_enabled;
    status.wifi_connected = connected;
    status.wifi_signal_level = status.network_signal_level;
    status.volume_supported = audio_initialized_.load();
    status.volume_percent = volume();
    (void)pxsys_system_status_service_update(
        pxsys_standard_system_status(system_), &status);
}

void Esp32S31Korvo1Hardware::StatusTimer(void* context) {
    static_cast<Esp32S31Korvo1Hardware*>(context)->ScheduleStatusUpdate();
}

void Esp32S31Korvo1Hardware::StatusOnLvgl(void* context) {
    auto* self = static_cast<Esp32S31Korvo1Hardware*>(context);
    self->status_update_pending_.store(false);
    self->PublishStatus();
}
