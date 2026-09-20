#include "te_sync_panel.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lcd_panel_interface.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "parallel_sw_rotation_flush.h"

#include <cstdint>
#include <inttypes.h>

namespace {

constexpr char kTag[] = "te_sync_panel";
constexpr TickType_t kTeWaitTimeout = pdMS_TO_TICKS(45);
constexpr uint32_t kTimeoutLogInterval = 60;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
constexpr uint32_t kDiagnosticsReportIntervalUs = 2 * 1000 * 1000;
#endif

struct TeSyncPanel {
    esp_lcd_panel_t base;
    esp_lcd_panel_handle_t delegate;
    SemaphoreHandle_t te_semaphore;
    StaticSemaphore_t te_semaphore_storage;
    gpio_num_t te_gpio;
    int width;
    int height;
    uint32_t update_interval_te_edges;
    uint32_t timeout_count;
    volatile uint32_t te_sequence;
    uint32_t last_update_te_sequence;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    uint32_t timeout_count_in_window;
    volatile uint32_t te_interrupt_count;
    uint32_t synchronized_update_count;
    uint32_t report_started_us;
    uint32_t te_wait_total_us;
    uint32_t te_wait_max_us;
    uint32_t submit_total_us;
    uint32_t submit_max_us;
#endif
    bool isr_registered;
};

TeSyncPanel* GetContext(esp_lcd_panel_t* panel) {
    return reinterpret_cast<TeSyncPanel*>(panel);
}

void IRAM_ATTR TeInterruptHandler(void* arg) {
    auto* context = static_cast<TeSyncPanel*>(arg);
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    context->te_interrupt_count = context->te_interrupt_count + 1;
#endif
    context->te_sequence = context->te_sequence + 1;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(context->te_semaphore, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

bool SequenceReached(uint32_t current, uint32_t target) {
    return static_cast<int32_t>(current - target) >= 0;
}

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
uint32_t RateX10(uint32_t count, uint32_t interval_us) {
    return interval_us == 0 ? 0 : static_cast<uint32_t>(
        count * 10ULL * 1000ULL * 1000ULL / interval_us);
}

void LogDiagnostics(TeSyncPanel* context, uint32_t now_us) {
    const uint32_t interval_us = now_us - context->report_started_us;
    if (interval_us < kDiagnosticsReportIntervalUs) {
        return;
    }

    const uint32_t te_interrupts = context->te_interrupt_count;
    context->te_interrupt_count = 0;
    const uint32_t update_rate_x10 = RateX10(
        context->synchronized_update_count, interval_us);
    const uint32_t te_rate_x10 = RateX10(te_interrupts, interval_us);
    const uint32_t average_wait_us = context->synchronized_update_count == 0 ? 0 :
        context->te_wait_total_us / context->synchronized_update_count;
    const uint32_t average_submit_us = context->synchronized_update_count == 0 ? 0 :
        context->submit_total_us / context->synchronized_update_count;

    if (zuowei_pai_touch::ParallelSoftwareRotationFlush::
            PerformanceLogEnabled()) {
    ESP_LOGI(kTag,
             "TE perf: window=%" PRIu32 "ms updates=%" PRIu32
             " (%" PRIu32 ".%uHz) te_irq=%" PRIu32
             " (%" PRIu32 ".%uHz) divider=%" PRIu32
             " timeout=%" PRIu32,
             interval_us / 1000, context->synchronized_update_count,
             update_rate_x10 / 10, static_cast<unsigned>(update_rate_x10 % 10),
             te_interrupts, te_rate_x10 / 10,
             static_cast<unsigned>(te_rate_x10 % 10),
             context->update_interval_te_edges,
             context->timeout_count_in_window);
    ESP_LOGI(kTag,
             "TE phase us avg/max: wait_edge=%" PRIu32 "/%" PRIu32
             " submit=%" PRIu32 "/%" PRIu32,
             average_wait_us, context->te_wait_max_us, average_submit_us,
             context->submit_max_us);
    }

    context->report_started_us = now_us;
    context->timeout_count_in_window = 0;
    context->synchronized_update_count = 0;
    context->te_wait_total_us = 0;
    context->te_wait_max_us = 0;
    context->submit_total_us = 0;
    context->submit_max_us = 0;
}
#endif

esp_err_t PanelReset(esp_lcd_panel_t* panel) {
    return esp_lcd_panel_reset(GetContext(panel)->delegate);
}

esp_err_t PanelInit(esp_lcd_panel_t* panel) {
    return esp_lcd_panel_init(GetContext(panel)->delegate);
}

esp_err_t PanelDelete(esp_lcd_panel_t* panel) {
    auto* context = GetContext(panel);
    const esp_lcd_panel_handle_t delegate = context->delegate;

    if (context->isr_registered) {
        gpio_intr_disable(context->te_gpio);
        gpio_isr_handler_remove(context->te_gpio);
    }
    gpio_reset_pin(context->te_gpio);
    vSemaphoreDelete(context->te_semaphore);
    heap_caps_free(context);

    return esp_lcd_panel_del(delegate);
}

esp_err_t PanelDrawBitmap(esp_lcd_panel_t* panel, int x_start, int y_start,
                          int x_end, int y_end, const void* color_data) {
    auto* context = GetContext(panel);
    // LVGL retains logical dimensions after a 90/270 degree hardware rotation,
    // while the panel scan dimensions remain physical.
    const bool matches_native_frame = x_end == context->width &&
                                      y_end == context->height;
    const bool matches_rotated_frame = x_end == context->height &&
                                       y_end == context->width;
    const bool is_full_frame = x_start == 0 && y_start == 0 &&
                               (matches_native_frame || matches_rotated_frame);
    const bool synchronize_to_te = is_full_frame;

    if (synchronize_to_te) {
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        const uint32_t wait_started_us =
            static_cast<uint32_t>(esp_timer_get_time());
#endif
        // Drop a stale semaphore token, then select a future TE sequence.
        xSemaphoreTake(context->te_semaphore, 0);
        const uint32_t current_sequence = context->te_sequence;
        uint32_t target_sequence = current_sequence + 1;
        if (context->last_update_te_sequence != 0) {
            const uint32_t scheduled_sequence =
                context->last_update_te_sequence +
                context->update_interval_te_edges;
            if (!SequenceReached(current_sequence, scheduled_sequence)) {
                target_sequence = scheduled_sequence;
            }
        }

        bool edge_received = false;
        while (!SequenceReached(context->te_sequence, target_sequence)) {
            if (xSemaphoreTake(context->te_semaphore,
                               kTeWaitTimeout) != pdTRUE) {
                break;
            }
        }
        edge_received = SequenceReached(context->te_sequence, target_sequence);
        if (!edge_received) {
            ++context->timeout_count;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
            ++context->timeout_count_in_window;
#endif
            if (context->timeout_count == 1 ||
                context->timeout_count % kTimeoutLogInterval == 0) {
                ESP_LOGW(kTag, "Timed out waiting for TE on GPIO%d (%lu times)",
                         static_cast<int>(context->te_gpio),
                         static_cast<unsigned long>(context->timeout_count));
            }
        } else {
            context->timeout_count = 0;
            context->last_update_te_sequence = context->te_sequence;
        }

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        const uint32_t wait_us = static_cast<uint32_t>(esp_timer_get_time()) -
            wait_started_us;
        context->te_wait_total_us += wait_us;
        if (wait_us > context->te_wait_max_us) {
            context->te_wait_max_us = wait_us;
        }
#endif
    }

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    const uint32_t submit_started_us = synchronize_to_te ?
        static_cast<uint32_t>(esp_timer_get_time()) : 0;
#endif
    const esp_err_t ret = esp_lcd_panel_draw_bitmap(
        context->delegate, x_start, y_start, x_end, y_end, color_data);
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    if (synchronize_to_te) {
        const uint32_t submit_us = static_cast<uint32_t>(esp_timer_get_time()) -
            submit_started_us;
        ++context->synchronized_update_count;
        context->submit_total_us += submit_us;
        if (submit_us > context->submit_max_us) {
            context->submit_max_us = submit_us;
        }
        LogDiagnostics(context, static_cast<uint32_t>(esp_timer_get_time()));
    }
#endif
    return ret;
}

esp_err_t PanelMirror(esp_lcd_panel_t* panel, bool mirror_x, bool mirror_y) {
    return esp_lcd_panel_mirror(GetContext(panel)->delegate, mirror_x, mirror_y);
}

esp_err_t PanelSwapXY(esp_lcd_panel_t* panel, bool swap_axes) {
    return esp_lcd_panel_swap_xy(GetContext(panel)->delegate, swap_axes);
}

esp_err_t PanelSetGap(esp_lcd_panel_t* panel, int x_gap, int y_gap) {
    return esp_lcd_panel_set_gap(GetContext(panel)->delegate, x_gap, y_gap);
}

esp_err_t PanelInvertColor(esp_lcd_panel_t* panel, bool invert_color_data) {
    return esp_lcd_panel_invert_color(GetContext(panel)->delegate,
                                      invert_color_data);
}

esp_err_t PanelDisplayOnOff(esp_lcd_panel_t* panel, bool on_off) {
    return esp_lcd_panel_disp_on_off(GetContext(panel)->delegate, on_off);
}

esp_err_t PanelDisplaySleep(esp_lcd_panel_t* panel, bool sleep) {
    return esp_lcd_panel_disp_sleep(GetContext(panel)->delegate, sleep);
}

esp_err_t PanelSetBrightness(esp_lcd_panel_t* panel, int brightness) {
    return esp_lcd_panel_set_brightness(GetContext(panel)->delegate, brightness);
}

void CleanupFailedCreate(TeSyncPanel* context) {
    if (context->isr_registered) {
        gpio_intr_disable(context->te_gpio);
        gpio_isr_handler_remove(context->te_gpio);
    }
    if (context->te_semaphore != nullptr) {
        vSemaphoreDelete(context->te_semaphore);
    }
    heap_caps_free(context);
}

}  // namespace

esp_err_t CreateTeSynchronizedPanel(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_handle_t panel,
                                    gpio_num_t te_gpio,
                                    int width,
                                    int height,
                                    uint32_t update_interval_te_edges,
                                    esp_lcd_panel_handle_t* out_panel) {
    if (panel_io == nullptr || panel == nullptr || te_gpio == GPIO_NUM_NC ||
        width <= 0 || height <= 0 || update_interval_te_edges == 0 ||
        out_panel == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    auto* context = static_cast<TeSyncPanel*>(
        heap_caps_calloc(1, sizeof(TeSyncPanel),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (context == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    context->delegate = panel;
    context->te_gpio = te_gpio;
    context->width = width;
    context->height = height;
    context->update_interval_te_edges = update_interval_te_edges;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    context->report_started_us = static_cast<uint32_t>(esp_timer_get_time());
#endif
    context->te_semaphore =
        xSemaphoreCreateBinaryStatic(&context->te_semaphore_storage);
    if (context->te_semaphore == nullptr) {
        CleanupFailedCreate(context);
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t te_gpio_config = {};
    te_gpio_config.pin_bit_mask = 1ULL << static_cast<uint32_t>(te_gpio);
    te_gpio_config.mode = GPIO_MODE_INPUT;
    te_gpio_config.pull_up_en = GPIO_PULLUP_DISABLE;
    te_gpio_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    te_gpio_config.intr_type = GPIO_INTR_POSEDGE;

    esp_err_t ret = gpio_config(&te_gpio_config);
    if (ret != ESP_OK) {
        CleanupFailedCreate(context);
        return ret;
    }

    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        CleanupFailedCreate(context);
        return ret;
    }

    ret = gpio_isr_handler_add(te_gpio, TeInterruptHandler, context);
    if (ret != ESP_OK) {
        CleanupFailedCreate(context);
        return ret;
    }
    context->isr_registered = true;

    const uint8_t te_mode = 0x00;  // V-blanking information only.
    ret = esp_lcd_panel_io_tx_param(panel_io, LCD_CMD_TEON, &te_mode,
                                    sizeof(te_mode));
    if (ret != ESP_OK) {
        CleanupFailedCreate(context);
        return ret;
    }

    context->base.reset = PanelReset;
    context->base.init = PanelInit;
    context->base.del = PanelDelete;
    context->base.draw_bitmap = PanelDrawBitmap;
    context->base.mirror = PanelMirror;
    context->base.swap_xy = PanelSwapXY;
    context->base.set_gap = PanelSetGap;
    context->base.invert_color = PanelInvertColor;
    context->base.disp_on_off = PanelDisplayOnOff;
    context->base.disp_sleep = PanelDisplaySleep;
    context->base.set_brightness = PanelSetBrightness;
    context->base.user_data = nullptr;

    *out_panel = &context->base;
    ESP_LOGI(kTag,
             "LCD TE synchronization enabled on GPIO%d, update every %" PRIu32
             " edge(s)",
             static_cast<int>(te_gpio), update_interval_te_edges);
    return ESP_OK;
}
