#pragma once

#include <esp_attr.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#if CONFIG_PXA_ENABLED
#include <pxa/pxa_esp_surface.h>
#include <pxa/pxa_surface_transform.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <inttypes.h>
#include <new>

namespace zuowei_pai_touch {

// Direct-mode RGB565 output for the board's fixed 270-degree orientation.
// LVGL only redraws dirty logical regions. On the last flush, two cores rotate
// the complete logical frame into one of two physical buffers. A separate task
// serializes TE-synchronized SPI transfers. Rotation can fill one physical
// buffer while the panel IO owns the other; IO completion releases only the
// submitted physical buffer.
class ParallelSoftwareRotationFlush {
private:
    struct Context;

public:
    struct CompletedFrameInfo {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride_bytes = 0;
        uint64_t frame_id = 0;
        uint64_t completed_timestamp_us = 0;
        const char* source = "unknown";
    };

    // Direct presenters bypass LVGL while retaining this class's rotation,
    // output-buffer queue and SPI ownership. The source is released once both
    // rotation halves have copied it into a board-owned output buffer.
    using DirectFrameReleaseCallback = void (*)(void* context,
                                                uint8_t buffer_index);

    static bool Install(lv_display_t* display,
                        esp_lcd_panel_io_handle_t panel_io,
                        esp_lcd_panel_handle_t panel) {
        if (display == nullptr || panel_io == nullptr || panel == nullptr ||
            GetContext() != nullptr ||
            lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_270 ||
            lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) {
            return false;
        }

        const int32_t logical_width =
            lv_display_get_horizontal_resolution(display);
        const int32_t logical_height =
            lv_display_get_vertical_resolution(display);
        if (logical_width <= 0 || logical_height <= 0) {
            return false;
        }

        void* context_memory = heap_caps_calloc(
            1, sizeof(Context), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        auto* context = context_memory == nullptr ? nullptr :
            new (context_memory) Context;
        if (context == nullptr) {
            ESP_LOGE(kTag, "Unable to allocate rotation context");
            return false;
        }

        context->display = display;
        context->panel_io = panel_io;
        context->panel = panel;
        context->logical_width = logical_width;
        context->logical_height = logical_height;
        context->native_width = logical_height;
        context->native_height = logical_width;
        context->output_bytes = static_cast<size_t>(logical_width) *
                                static_cast<size_t>(logical_height) *
                                sizeof(uint16_t);
        context->split_column = AlignSplit(logical_width / 2, logical_width);
        context->report_started_us =
            static_cast<uint32_t>(esp_timer_get_time());

        for (uint8_t i = 0; i < kOutputBufferCount; ++i) {
            /* SPI reads these buffers directly from PSRAM. Keep every frame
             * and 4-line transfer boundary cache aligned so the driver never
             * needs a private DMA bounce buffer. */
            context->outputs[i] = static_cast<uint16_t*>(
                heap_caps_aligned_alloc(kOutputDmaAlignment,
                                        context->output_bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (context->outputs[i] == nullptr) {
                ESP_LOGE(kTag, "Unable to allocate output buffer %u (%uB)",
                         static_cast<unsigned>(i),
                         static_cast<unsigned>(context->output_bytes));
                Cleanup(context);
                return false;
            }
        }

        context->worker_done = xSemaphoreCreateBinaryStatic(
            &context->worker_done_storage);
        context->rotation_lock = xSemaphoreCreateMutexStatic(
            &context->rotation_lock_storage);
        context->transfer_done = xSemaphoreCreateBinaryStatic(
            &context->transfer_done_storage);
        context->free_queue = xQueueCreateStatic(
            kOutputBufferCount, sizeof(uint8_t), context->free_queue_storage,
            &context->free_queue_struct);
        context->ready_queue = xQueueCreateStatic(
            kReadyQueueLength, sizeof(uint8_t), context->ready_queue_storage,
            &context->ready_queue_struct);
        if (context->worker_done == nullptr || context->rotation_lock == nullptr ||
            context->transfer_done == nullptr ||
            context->free_queue == nullptr || context->ready_queue == nullptr) {
            ESP_LOGE(kTag, "Unable to create rotation synchronization objects");
            Cleanup(context);
            return false;
        }

        for (uint8_t i = 0; i < kOutputBufferCount; ++i) {
            xQueueSend(context->free_queue, &i, 0);
        }

#if !CONFIG_FREERTOS_UNICORE
        if (xTaskCreatePinnedToCore(WorkerTask, "lcd_rotate", kWorkerStackSize,
                                    context, kWorkerPriority,
                                    &context->worker_task, kWorkerCore) == pdPASS) {
            context->uses_worker = true;
        } else {
            ESP_LOGW(kTag, "Rotation worker creation failed; using one core");
        }
#endif

        if (xTaskCreate(SubmitTask, "lcd_submit", kSubmitStackSize, context,
                        kSubmitPriority, &context->submit_task) != pdPASS) {
            ESP_LOGE(kTag, "Unable to create LCD submit task");
            Cleanup(context);
            return false;
        }
#if CONFIG_PXA_ENABLED
        if (xTaskCreate(PxaPresenterTask, "pxa_present", kPresenterStackSize,
                        context, kPresenterPriority,
                        &context->pxa_presenter_task) != pdPASS) {
            ESP_LOGE(kTag, "Unable to create PXA Presenter task");
            Cleanup(context);
            return false;
        }
#endif

        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = ColorTransferDoneCallback,
        };
        const esp_err_t callback_result =
            esp_lcd_panel_io_register_event_callbacks(panel_io, &callbacks,
                                                       context);
        if (callback_result != ESP_OK) {
            ESP_LOGE(kTag, "Unable to install LCD completion callback: %s",
                     esp_err_to_name(callback_result));
            Cleanup(context);
            return false;
        }

        GetContext() = context;
#if CONFIG_PXA_ENABLED
        // Rotation handles integer-scaled direct frames. Keep native output as
        // the board default while allowing GameRender apps to request 2x.
        (void)pxa_esp_game_render_set_scale_profile(
            PXA_GAME_RENDER_SCALE_MASK_1X |
                PXA_GAME_RENDER_SCALE_MASK_2X,
            PXA_GAME_RENDER_SCALE_1X);
        pxa_esp_surface_set_frame_ready_callback(SurfaceFrameReady, context);
#endif
        lv_display_set_flush_cb(display, FlushCallback);
        ESP_LOGI(kTag,
                 "Direct pipelined rotation installed: logical=%ldx%ld native=%ldx%ld "
                 "worker_core=%d active=%d output_buffers=%u buffer_bytes=%u",
                 static_cast<long>(logical_width),
                 static_cast<long>(logical_height),
                 static_cast<long>(context->native_width),
                 static_cast<long>(context->native_height), kWorkerCore,
                 static_cast<int>(context->uses_worker),
                 static_cast<unsigned>(kOutputBufferCount),
                 static_cast<unsigned>(context->output_bytes));
        return true;
    }

    static bool SubmitDirectFrame(const uint16_t* pixels, uint32_t width,
                                  uint32_t height, bool byte_swapped,
                                  uint8_t buffer_index,
                                  uint64_t frame_id,
                                  uint64_t input_timestamp_us,
                                  DirectFrameReleaseCallback release,
                                  void* release_context) {
        auto* context = GetContext();
        const uint8_t scale =
            context == nullptr ? 0 : DirectFrameScale(context, width, height);
        if (context == nullptr || pixels == nullptr || scale == 0) {
            return false;
        }
        if (xSemaphoreTake(context->rotation_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            return false;
        }
        if (!context->direct_scanout_active) {
            xSemaphoreGive(context->rotation_lock);
            return false;
        }

        uint8_t output_index = 0;
        if (!AcquireOutputBuffer(context, &output_index)) {
            xSemaphoreGive(context->rotation_lock);
            return false;
        }
        uint16_t* output = context->outputs[output_index];
#if CONFIG_PXA_ENABLED
        const uint32_t rotation_started_us =
            static_cast<uint32_t>(esp_timer_get_time());
#endif
        bool copied = RotateDirectFrame(context, pixels, width, height, scale,
                                        output, byte_swapped);
#if CONFIG_PXA_ENABLED
        const uint32_t rotation_us =
            static_cast<uint32_t>(esp_timer_get_time()) - rotation_started_us;
#endif
        if (copied) {
            CompositePerformanceOverlay(context, output);
            context->output_frame_id[output_index] = frame_id;
            context->output_source[output_index] = kFrameSourceSurface;
            context->output_input_timestamp_us[output_index] =
                input_timestamp_us;
            if (xQueueSend(context->ready_queue, &output_index, 0) != pdTRUE) {
                context->output_input_timestamp_us[output_index] = 0;
                copied = false;
            }
        }
        if (!copied) {
            (void)xQueueSend(context->free_queue, &output_index, 0);
#if CONFIG_PXA_ENABLED
            ++context->pxa_direct_submit_failures;
#endif
        } else {
#if CONFIG_PXA_ENABLED
            ++context->pxa_direct_frame_count;
            context->pxa_direct_rotate_total_us += rotation_us;
            if (rotation_us > context->pxa_direct_rotate_max_us) {
                context->pxa_direct_rotate_max_us = rotation_us;
            }
#endif
        }
        xSemaphoreGive(context->rotation_lock);

        if (copied) {
            LogDiagnostics(context,
                           static_cast<uint32_t>(esp_timer_get_time()));
        }

        if (copied && release != nullptr) {
            release(release_context, buffer_index);
        }
        return copied;
    }

    static bool BeginDirectScanout(TickType_t timeout) {
        auto* context = GetContext();
        if (context == nullptr ||
            xSemaphoreTake(context->rotation_lock, timeout) != pdTRUE) {
            return false;
        }
        context->direct_scanout_active = true;
        xSemaphoreGive(context->rotation_lock);

        // Suppress later LVGL flushes before waiting for the old frame to
        // drain. Otherwise a queued LVGL frame could follow a direct frame.
        if (WaitForPendingTransfers(timeout)) {
            return true;
        }
        (void)EndDirectScanout(timeout);
        return false;
    }

    static bool EndDirectScanout(TickType_t timeout) {
        auto* context = GetContext();
        if (context == nullptr ||
            xSemaphoreTake(context->rotation_lock, timeout) != pdTRUE) {
            return false;
        }
        context->direct_scanout_active = false;
        xSemaphoreGive(context->rotation_lock);
        return true;
    }

    static bool GetDirectSurfaceSize(uint32_t* width, uint32_t* height) {
        auto* context = GetContext();
        if (context == nullptr || width == nullptr || height == nullptr) {
            return false;
        }
        *width = static_cast<uint32_t>(context->logical_width);
        *height = static_cast<uint32_t>(context->logical_height);
        return true;
    }

    /* Copies the most recently completed SPI output frame into logical
     * row-major RGB565 host byte order. This captures direct Surface, composed
     * and LVGL frames without forcing a compositor transition. */
    static bool SnapshotCompletedFrame(uint16_t* output, size_t pixel_count,
                                       bool after_present,
                                       TickType_t timeout,
                                       CompletedFrameInfo* info) {
        auto* context = GetContext();
        if (context == nullptr || output == nullptr || info == nullptr ||
            pixel_count != context->output_bytes / sizeof(uint16_t)) {
            return false;
        }
        const TickType_t started = xTaskGetTickCount();
        const uint32_t initial_sequence =
            context->completed_sequence.load(std::memory_order_acquire);
        if (after_present && context->display != nullptr) {
            /* A static LVGL screen may otherwise never produce a frame after
             * the capture request. Direct scanout suppresses this flush, so
             * an active game still satisfies the request with its next real
             * Surface frame without handing compositor ownership to LVGL. */
            lv_lock();
            lv_obj_t* screen =
                lv_display_get_screen_active(context->display);
            if (screen != nullptr) lv_obj_invalidate(screen);
            lv_refr_now(context->display);
            lv_unlock();
        }
        while (after_present &&
               context->completed_sequence.load(std::memory_order_acquire) ==
                   initial_sequence) {
            if (xTaskGetTickCount() - started >= timeout) return false;
            vTaskDelay(1);
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        const TickType_t remaining = elapsed < timeout ? timeout - elapsed : 0;
        if (xSemaphoreTake(context->rotation_lock, remaining) != pdTRUE)
            return false;
        bool copied = false;
        if (WaitForPendingTransfers(remaining)) {
            const int8_t index = context->last_completed_output.load(
                std::memory_order_acquire);
            if (index >= 0 && index < kOutputBufferCount) {
                const uint16_t* physical = context->outputs[index];
                copied = pxa_surface_capture_rgb565_270_to_logical(
                    physical, output,
                    static_cast<uint16_t>(context->logical_width),
                    static_cast<uint16_t>(context->logical_height),
                    static_cast<uint32_t>(context->logical_height),
                    static_cast<uint32_t>(context->logical_width), true);
                if (copied) {
                    info->width =
                        static_cast<uint32_t>(context->logical_width);
                    info->height =
                        static_cast<uint32_t>(context->logical_height);
                    info->stride_bytes = static_cast<uint32_t>(
                        context->logical_width * sizeof(uint16_t));
                    info->frame_id = context->last_completed_frame_id.load(
                        std::memory_order_acquire);
                    info->completed_timestamp_us =
                        context->last_completed_timestamp_us.load(
                            std::memory_order_acquire);
                    info->source = FrameSourceName(
                        context->last_completed_source.load(
                            std::memory_order_acquire));
                }
            }
        }
        xSemaphoreGive(context->rotation_lock);
        return copied;
    }

    // LVGL marks a flush ready after rotation so it can render the next frame,
    // while the physical buffer can still be waiting for TE or SPI completion.
    // Call this after a forced refresh when the panel must contain that frame
    // before another hardware action, such as enabling the backlight.
    static bool WaitForPendingTransfers(TickType_t timeout) {
        auto* context = GetContext();
        if (context == nullptr) return false;

        const TickType_t started = xTaskGetTickCount();
        do {
            if (uxQueueMessagesWaiting(context->free_queue) ==
                    kOutputBufferCount &&
                uxQueueMessagesWaiting(context->ready_queue) == 0) {
                return true;
            }
            vTaskDelay(1);
        } while (xTaskGetTickCount() - started < timeout);

        return uxQueueMessagesWaiting(context->free_queue) ==
                   kOutputBufferCount &&
               uxQueueMessagesWaiting(context->ready_queue) == 0;
    }

private:
    static constexpr char kTag[] = "FastRotate";
    static constexpr uint8_t kOutputBufferCount = 2;
    static constexpr uint8_t kReadyQueueLength = 1;
    static constexpr size_t kOutputDmaAlignment = 64;
    // LVGL is pinned to CPU1. Keep this half-frame worker on CPU0 so the two
    // halves rotate concurrently; the submit task remains unpinned and takes
    // priority when a TE-synchronized transfer is ready.
    static constexpr BaseType_t kWorkerCore = 0;
    static constexpr UBaseType_t kWorkerPriority = 4;
    // TE-triggered submission must outrank both LVGL and the rotation worker.
    // Otherwise their PSRAM traffic stretches a 15ms transfer beyond the
    // panel's safe scan window.
    static constexpr UBaseType_t kSubmitPriority = 5;
    static constexpr UBaseType_t kPresenterPriority = 4;
    static constexpr uint32_t kWorkerStackSize = 2048;
    static constexpr uint32_t kSubmitStackSize = 3072;
    static constexpr uint32_t kPresenterStackSize = 3072;
    static constexpr uint32_t kReportIntervalUs = 2 * 1000 * 1000;
    static constexpr TickType_t kWorkerDoneTimeout = pdMS_TO_TICKS(500);
    static constexpr TickType_t kTransferDoneTimeout = pdMS_TO_TICKS(50);
    static constexpr TickType_t kTransferErrorDrainDelay = pdMS_TO_TICKS(20);
    static constexpr TickType_t kPxaDirectTransitionTimeout = pdMS_TO_TICKS(150);
    static constexpr int32_t kTileSize = 16;
    static constexpr int32_t kMinimumColumnsPerCore = 64;
    static constexpr uint32_t kAdaptThresholdUs = 300;
    static constexpr uint8_t kFrameSourceLvgl = 0;
    static constexpr uint8_t kFrameSourceSurface = 1;
    static constexpr uint8_t kFrameSourceComposed = 2;

    struct Context {
        lv_display_t* display = nullptr;
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        uint16_t* outputs[kOutputBufferCount] = {};
        size_t output_bytes = 0;
        int32_t logical_width = 0;
        int32_t logical_height = 0;
        int32_t native_width = 0;
        int32_t native_height = 0;

        SemaphoreHandle_t worker_done = nullptr;
        StaticSemaphore_t worker_done_storage = {};
        SemaphoreHandle_t rotation_lock = nullptr;
        StaticSemaphore_t rotation_lock_storage = {};
        SemaphoreHandle_t transfer_done = nullptr;
        StaticSemaphore_t transfer_done_storage = {};
        QueueHandle_t free_queue = nullptr;
        StaticQueue_t free_queue_struct = {};
        alignas(4) uint8_t free_queue_storage[
            kOutputBufferCount * sizeof(uint8_t)] = {};
        QueueHandle_t ready_queue = nullptr;
        StaticQueue_t ready_queue_struct = {};
        alignas(4) uint8_t ready_queue_storage[
            kReadyQueueLength * sizeof(uint8_t)] = {};

        TaskHandle_t worker_task = nullptr;
        TaskHandle_t submit_task = nullptr;
        const uint16_t* source = nullptr;
        uint16_t* job_output = nullptr;
        bool source_is_direct = false;
        bool source_byte_swapped = false;
        uint16_t direct_source_width = 0;
        uint16_t direct_source_height = 0;
        uint8_t direct_scale = 1;
        bool direct_scanout_active = false;
        std::atomic<int8_t> last_completed_output{-1};
        uint64_t output_frame_id[kOutputBufferCount] = {};
        uint8_t output_source[kOutputBufferCount] = {};
        uint64_t output_input_timestamp_us[kOutputBufferCount] = {};
        std::atomic<uint32_t> completed_sequence{0};
        std::atomic<uint64_t> last_completed_frame_id{0};
        std::atomic<uint64_t> last_completed_timestamp_us{0};
        std::atomic<uint8_t> last_completed_source{kFrameSourceLvgl};
        std::atomic<bool> performance_overlay_enabled{false};
        std::atomic<bool> performance_log_enabled{false};
        std::atomic<uint32_t> performance_fps_x10{0};
        std::atomic<uint32_t> performance_rotate_us{0};
        std::atomic<uint32_t> performance_send_us{0};
        int32_t split_column = 0;
        uint32_t worker_last_us = 0;
        bool uses_worker = false;
#if CONFIG_PXA_ENABLED
        TaskHandle_t pxa_presenter_task = nullptr;
        pxa_esp_surface_frame_t surface_frame = {};
        bool has_surface_frame = false;
        bool pxa_direct_scanout_active = false;
        uint64_t pxa_direct_lease = 0;
        uint32_t pxa_direct_frame_count = 0;
        uint32_t pxa_direct_rotate_total_us = 0;
        uint32_t pxa_direct_rotate_max_us = 0;
        uint32_t pxa_direct_submit_failures = 0;
        uint32_t pxa_direct_composition_fallbacks = 0;
        std::atomic<uint32_t> pxa_visible_frames{0};
        std::atomic<uint32_t> pxa_visible_interval_max_us{0};
        std::atomic<uint32_t> pxa_visible_min_1s_fps_x10{UINT32_MAX};
        uint64_t pxa_visible_last_us = 0;
        uint64_t pxa_visible_window_started_us = 0;
        uint32_t pxa_visible_window_frames = 0;
#endif

        uint32_t report_started_us = 0;
        uint32_t frame_count = 0;
        uint32_t flush_area_count = 0;
        uint32_t rotate_total_us = 0;
        uint32_t rotate_max_us = 0;
        uint32_t main_total_us = 0;
        uint32_t worker_total_us = 0;
        uint32_t dropped_queued_frames = 0;
        uint32_t dropped_no_buffer_frames = 0;

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        std::atomic<uint32_t> submitted_frames{0};
        std::atomic<uint32_t> completed_frames{0};
        std::atomic<uint32_t> submit_errors{0};
        std::atomic<uint32_t> transfer_timeouts{0};
        std::atomic<uint32_t> send_total_us{0};
        std::atomic<uint32_t> send_max_us{0};
#endif
    };

public:
    static bool PerformanceOverlayEnabled() {
        auto* context = GetContext();
        return context != nullptr && context->performance_overlay_enabled.load(
            std::memory_order_relaxed);
    }

    static void SetPerformanceOverlayEnabled(bool enabled) {
        auto* context = GetContext();
        if (context != nullptr)
            context->performance_overlay_enabled.store(enabled,
                                                       std::memory_order_relaxed);
    }

    static bool PerformanceLogEnabled() {
        auto* context = GetContext();
        return context != nullptr && context->performance_log_enabled.load(
            std::memory_order_relaxed);
    }

    static void SetPerformanceLogEnabled(bool enabled) {
        auto* context = GetContext();
        if (context != nullptr)
            context->performance_log_enabled.store(enabled,
                                                   std::memory_order_relaxed);
    }

private:
    static Context*& GetContext() {
        static Context* context = nullptr;
        return context;
    }

    static int32_t AlignSplit(int32_t split, int32_t width) {
        split = (split / kTileSize) * kTileSize;
        if (split < kMinimumColumnsPerCore) {
            split = kMinimumColumnsPerCore;
        }
        const int32_t maximum = width - kMinimumColumnsPerCore;
        if (split > maximum) {
            split = maximum;
        }
        return split;
    }

    static uint8_t DirectFrameScale(const Context* context, uint32_t width,
                                    uint32_t height) {
        if (context == nullptr || width > UINT16_MAX || height > UINT16_MAX ||
            context->logical_width > UINT16_MAX ||
            context->logical_height > UINT16_MAX) {
            return 0;
        }
        return pxa_surface_integer_scale(
            static_cast<uint16_t>(width), static_cast<uint16_t>(height),
            static_cast<uint16_t>(context->logical_width),
            static_cast<uint16_t>(context->logical_height));
    }

    static uint8_t GlyphRow(char character, uint8_t row) {
        static const uint8_t digits[][5] = {
            {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7},
            {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
            {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2}, {7, 5, 7, 5, 7},
            {7, 5, 7, 1, 7},
        };
        static const uint8_t f[] = {7, 4, 6, 4, 4};
        static const uint8_t p[] = {6, 5, 6, 4, 4};
        static const uint8_t s[] = {7, 4, 7, 1, 7};
        static const uint8_t r[] = {6, 5, 6, 5, 5};
        static const uint8_t m[] = {5, 7, 7, 5, 5};
        if (row >= 5) return 0;
        if (character >= '0' && character <= '9')
            return digits[character - '0'][row];
        switch (character) {
            case 'F': return f[row];
            case 'P': return p[row];
            case 'S': return s[row];
            case 'R': return r[row];
            case 'M': return m[row];
            case '.': return row == 4 ? 2 : 0;
            default: return 0;
        }
    }

    static void OverlayPixel(Context* context, uint16_t* output, int32_t x,
                             int32_t y, uint16_t color) {
        if (x < 0 || y < 0 || x >= context->logical_width ||
            y >= context->logical_height)
            return;
        const int32_t physical_x = context->logical_height - 1 - y;
        const int32_t physical_y = x;
        output[static_cast<size_t>(physical_y) * context->native_width +
               physical_x] = __builtin_bswap16(color);
    }

    static void OverlayFill(Context* context, uint16_t* output, int32_t x,
                            int32_t y, int32_t width, int32_t height,
                            uint16_t color) {
        for (int32_t yy = y; yy < y + height; ++yy)
            for (int32_t xx = x; xx < x + width; ++xx)
                OverlayPixel(context, output, xx, yy, color);
    }

    static void OverlayText(Context* context, uint16_t* output, int32_t x,
                            int32_t y, const char* text, uint16_t color) {
        constexpr int32_t kScale = 2;
        for (; text != nullptr && *text != '\0'; ++text, x += 8) {
            for (uint8_t row = 0; row < 5; ++row) {
                const uint8_t bits = GlyphRow(*text, row);
                for (uint8_t column = 0; column < 3; ++column)
                    if ((bits & (1u << (2u - column))) != 0)
                        OverlayFill(context, output, x + column * kScale,
                                    y + row * kScale, kScale, kScale, color);
            }
        }
    }

    static void CompositePerformanceOverlay(Context* context, uint16_t* output) {
        if (context == nullptr || output == nullptr ||
            !context->performance_overlay_enabled.load(
                std::memory_order_relaxed))
            return;
        char line[40];
        const uint32_t fps = context->performance_fps_x10.load(
            std::memory_order_relaxed);
        const uint32_t rotate_us = context->performance_rotate_us.load(
            std::memory_order_relaxed);
        const uint32_t send_us = context->performance_send_us.load(
            std::memory_order_relaxed);
        snprintf(line, sizeof(line), "FPS%lu.%lu R%lu M%lu",
                 static_cast<unsigned long>(fps / 10),
                 static_cast<unsigned long>(fps % 10),
                 static_cast<unsigned long>(rotate_us / 1000),
                 static_cast<unsigned long>(send_us / 1000));
        const int32_t width = static_cast<int32_t>(strlen(line)) * 8 + 8;
        const int32_t x = (context->logical_width - width) / 2;
        OverlayFill(context, output, x, 0, width, 16, 0x0000);
        OverlayText(context, output, x + 4, 3, line, 0xffff);
    }

    static const char* FrameSourceName(uint8_t source) {
        switch (source) {
            case kFrameSourceSurface:
                return "surface";
            case kFrameSourceComposed:
                return "composed";
            default:
                return "lvgl";
        }
    }

    static int32_t DirectSplit(int32_t width) {
        int32_t split = ((width / 2) / 8) * 8;
        if (split < 8) split = 8;
        if (split > width - 8) split = width - 8;
        return split;
    }

    static void Cleanup(Context* context) {
        if (context == nullptr) {
            return;
        }
#if CONFIG_PXA_ENABLED
        if (context->pxa_presenter_task != nullptr) {
            vTaskDelete(context->pxa_presenter_task);
        }
#endif
        if (context->submit_task != nullptr) {
            vTaskDelete(context->submit_task);
        }
        if (context->worker_task != nullptr) {
            vTaskDelete(context->worker_task);
        }
        if (context->ready_queue != nullptr) {
            vQueueDelete(context->ready_queue);
        }
        if (context->free_queue != nullptr) {
            vQueueDelete(context->free_queue);
        }
        if (context->transfer_done != nullptr) {
            vSemaphoreDelete(context->transfer_done);
        }
        if (context->worker_done != nullptr) {
            vSemaphoreDelete(context->worker_done);
        }
        if (context->rotation_lock != nullptr) {
            vSemaphoreDelete(context->rotation_lock);
        }
        for (auto*& output : context->outputs) {
            if (output != nullptr) {
                heap_caps_free(output);
                output = nullptr;
            }
        }
        context->~Context();
        heap_caps_free(context);
    }

#if CONFIG_PXA_ENABLED
    static uint16_t BlendArgb8888Premultiplied(uint16_t destination,
                                                uint32_t argb) {
        const uint8_t alpha = static_cast<uint8_t>(argb >> 24);
        if (alpha == 0) return destination;
        const uint16_t source = static_cast<uint16_t>(
            ((argb >> 8) & 0xf800u) | ((argb >> 5) & 0x07e0u) |
            ((argb >> 3) & 0x001fu));
        if (alpha == 255) return source;
        const uint16_t inverse = static_cast<uint16_t>(255u - alpha);
        /* Use independent 16-bit lanes for red and blue before multiplying.
         * Their RGB565 bit positions cannot be multiplied together directly:
         * red's discarded fractional bits would otherwise leak into blue. */
        const uint32_t red_blue_lanes = (destination >> 11) |
            (static_cast<uint32_t>(destination & 0x001fu) << 16);
        const uint32_t retained_red_blue_lanes = red_blue_lanes * inverse;
        const uint16_t retained_red = static_cast<uint16_t>(
            ((retained_red_blue_lanes + 128u) >> 8) & 0x001fu);
        const uint16_t retained_blue = static_cast<uint16_t>(
            ((retained_red_blue_lanes >> 16) + 128u) >> 8) & 0x001fu;
        const uint16_t retained_green = static_cast<uint16_t>(
            ((static_cast<uint32_t>(destination & 0x07e0u) * inverse +
              (128u << 5)) >> 8) >> 5);
        return static_cast<uint16_t>(
            (((source >> 11) + retained_red) << 11) |
            ((((source >> 5) & 0x003fu) + retained_green) << 5) |
            ((source & 0x001fu) + retained_blue));
    }

    static uint16_t BlendSurfacePixel(uint16_t destination,
                                      const uint8_t* pixel,
                                      uint16_t format) {
        if (format == PXA_SURFACE_FORMAT_RGB565) {
            return static_cast<uint16_t>(pixel[0] | (pixel[1] << 8));
        }
        uint32_t argb;
        __builtin_memcpy(&argb, pixel, sizeof(argb));
        return BlendArgb8888Premultiplied(destination, argb);
    }

    static uint16_t BlendUiAlphaPixel(uint16_t destination,
                                      uint16_t foreground, uint8_t alpha,
                                      uint8_t opacity) {
        if (alpha == 0 || opacity == 0) return destination;
        if (opacity != 255) {
            alpha = static_cast<uint8_t>(
                (static_cast<uint16_t>(alpha) * opacity + 127u) / 255u);
            if (alpha == 0) return destination;
        }
        if (alpha == 255) return foreground;

        const uint16_t inverse = static_cast<uint16_t>(255u - alpha);
        const uint16_t red = static_cast<uint16_t>(
            (((foreground >> 11) * alpha + (destination >> 11) * inverse +
              128u) >> 8));
        const uint16_t green = static_cast<uint16_t>(
            (((((foreground >> 5) & 0x3fu) * alpha +
               ((destination >> 5) & 0x3fu) * inverse + 128u) >> 8)));
        const uint16_t blue = static_cast<uint16_t>(
            (((foreground & 0x1fu) * alpha + (destination & 0x1fu) * inverse +
              128u) >> 8));
        return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
    }

    static uint8_t SurfaceCompositionScale(
        const pxa_esp_surface_frame_t* surface, int32_t logical_width,
        int32_t logical_height) {
        if (surface == nullptr || surface->x != 0 || surface->y != 0 ||
            surface->width == 0 || surface->height == 0) {
            return 1;
        }
        if (logical_width > UINT16_MAX || logical_height > UINT16_MAX)
            return 1;
        const uint8_t scale = pxa_surface_integer_scale(
            surface->width, surface->height,
            static_cast<uint16_t>(logical_width),
            static_cast<uint16_t>(logical_height));
        return scale == 0 ? 1 : scale;
    }
#endif

    static void RotateColumns(const uint16_t* source, uint16_t* output,
                              int32_t source_width, int32_t source_height,
                              int32_t first_column, int32_t last_column
#if CONFIG_PXA_ENABLED
                              , const pxa_esp_surface_frame_t* surface
#endif
                              ) {
        alignas(16) uint16_t tile[kTileSize][kTileSize];
#if CONFIG_PXA_ENABLED
        const uint8_t surface_scale = SurfaceCompositionScale(
            surface, source_width, source_height);
#endif
        for (int32_t y0 = 0; y0 < source_height; y0 += kTileSize) {
            const int32_t y1 = (y0 + kTileSize < source_height) ?
                y0 + kTileSize : source_height;
            for (int32_t x0 = first_column; x0 < last_column; x0 += kTileSize) {
                const int32_t x1 = (x0 + kTileSize < last_column) ?
                    x0 + kTileSize : last_column;

                const int32_t tile_width = x1 - x0;
                const int32_t tile_height = y1 - y0;
                for (int32_t y = y0; y < y1; ++y) {
                    const uint16_t* pixel = source + y * source_width + x0;
                    uint16_t* tile_row = tile[y - y0];
#if CONFIG_PXA_ENABLED
                    int32_t overlay_first = x1;
                    int32_t overlay_last = x1;
                    const uint8_t* surface_row = nullptr;
                    uint8_t surface_bytes_per_pixel = 0;
                    if (surface != nullptr && surface->visible &&
                        static_cast<int64_t>(y) >= surface->y &&
                        static_cast<int64_t>(y) <
                            static_cast<int64_t>(surface->y) +
                                static_cast<int64_t>(surface->height) *
                                    surface_scale) {
                        const int64_t clipped_first =
                            surface->x > x0 ? surface->x : x0;
                        const int64_t surface_right =
                            static_cast<int64_t>(surface->x) +
                            static_cast<int64_t>(surface->width) *
                                surface_scale;
                        const int64_t clipped_last =
                            surface_right < x1 ? surface_right : x1;
                        if (clipped_first < clipped_last) {
                            overlay_first = static_cast<int32_t>(clipped_first);
                            overlay_last = static_cast<int32_t>(clipped_last);
                            surface_bytes_per_pixel =
                                surface->format == PXA_SURFACE_FORMAT_RGB565
                                    ? 2u : 4u;
                            surface_row = surface->pixels +
                                static_cast<size_t>(y - surface->y) /
                                    surface_scale * surface->stride_bytes;
                        }
                    }
                    int32_t x = x0;
                    const pxa_surface_damage_rect_t*
                        row_overlay_regions[PXA_SURFACE_MAX_OPAQUE_UI_REGIONS] = {};
                    uint8_t row_overlay_count = 0;
                    if (surface_row != nullptr) {
                        const int32_t surface_y =
                            (y - surface->y) / surface_scale;
                        for (uint8_t region = 0;
                             region < surface->opaque_ui_region_count; ++region) {
                            const auto& rect =
                                surface->opaque_ui_regions[region];
                            if (surface_y >= rect.y &&
                                surface_y < static_cast<int32_t>(rect.y) +
                                                rect.height)
                                row_overlay_regions[row_overlay_count++] = &rect;
                        }
                    }
                    for (; x < overlay_first; ++x) {
                        uint16_t result = *pixel++;
                        tile_row[x - x0] = __builtin_bswap16(result);
                    }
                    if (surface_row != nullptr && row_overlay_count == 0) {
                        if (surface->format == PXA_SURFACE_FORMAT_RGB565) {
                            for (; x < overlay_last; ++x) {
                                const auto* surface_pixel = surface_row +
                                    static_cast<size_t>(x - surface->x) /
                                        surface_scale *
                                        sizeof(uint16_t);
                                tile_row[x - x0] = __builtin_bswap16(
                                    static_cast<uint16_t>(surface_pixel[0] |
                                                          surface_pixel[1] << 8));
                                ++pixel;
                            }
                        } else {
                            for (; x < overlay_last; ++x) {
                                const auto* argb =
                                    reinterpret_cast<const uint32_t*>(
                                        surface_row) +
                                    static_cast<size_t>(x - surface->x) /
                                        surface_scale;
                                tile_row[x - x0] = __builtin_bswap16(
                                    BlendArgb8888Premultiplied(*pixel++,
                                                               *argb));
                            }
                        }
                    } else {
                        for (; x < overlay_last; ++x) {
                            const int32_t surface_x =
                                (x - surface->x) / surface_scale;
                            bool use_lvgl = false;
                            for (uint8_t region = 0; region < row_overlay_count;
                                 ++region) {
                                const auto& rect = *row_overlay_regions[region];
                                if (surface_x >= rect.x &&
                                    surface_x < static_cast<int32_t>(rect.x) +
                                                    rect.width) {
                                    use_lvgl = true;
                                    break;
                                }
                            }
                            uint16_t result = *pixel;
                            if (!use_lvgl) {
                                const auto* surface_pixel = surface_row +
                                    static_cast<size_t>(surface_x) *
                                        surface_bytes_per_pixel;
                                result = BlendSurfacePixel(
                                    result, surface_pixel, surface->format);
                            }
                            tile_row[x - x0] = __builtin_bswap16(result);
                            ++pixel;
                        }
                    }
                    for (; x < x1; ++x) {
                        uint16_t result = *pixel++;
                        tile_row[x - x0] = __builtin_bswap16(result);
                    }
                    if (surface != nullptr) {
                        const auto& plane = surface->ui_alpha_plane;
                        if (plane.visible && plane.opacity != 0 &&
                            plane.pixels != nullptr && plane.alpha != nullptr &&
                            static_cast<int64_t>(y) >= plane.y &&
                            static_cast<int64_t>(y) <
                                static_cast<int64_t>(plane.y) + plane.height) {
                            const int64_t plane_right =
                                static_cast<int64_t>(plane.x) + plane.width;
                            const int32_t alpha_first = static_cast<int32_t>(
                                plane.x > x0 ? plane.x : x0);
                            const int32_t alpha_last = static_cast<int32_t>(
                                plane_right < x1 ? plane_right : x1);
                            if (alpha_first < alpha_last) {
                                const int32_t plane_y = y - plane.y;
                                const auto* colors =
                                    reinterpret_cast<const uint16_t*>(
                                        reinterpret_cast<const uint8_t*>(
                                            plane.pixels) +
                                        static_cast<size_t>(plane_y) *
                                            plane.pixel_stride_bytes) +
                                    (alpha_first - plane.x);
                                const auto* alpha = plane.alpha +
                                    static_cast<size_t>(plane_y) *
                                        plane.alpha_stride_bytes +
                                    (alpha_first - plane.x);
                                for (int32_t alpha_x = alpha_first;
                                     alpha_x < alpha_last; ++alpha_x) {
                                    uint16_t* destination =
                                        &tile_row[alpha_x - x0];
                                    const uint16_t base =
                                        __builtin_bswap16(*destination);
                                    *destination = __builtin_bswap16(
                                        BlendUiAlphaPixel(
                                            base, *colors++, *alpha++,
                                            plane.opacity));
                                }
                            }
                        }
                    }
#else
                    for (int32_t x = 0; x < tile_width; ++x) {
                        tile_row[x] = __builtin_bswap16(*pixel++);
                    }
#endif
                }

                // Transpose the internal tile into contiguous destination
                // columns. Both PSRAM reads and writes are now burst-friendly.
                for (int32_t x = 0; x < tile_width; ++x) {
                    uint16_t* destination =
                        output + (x0 + x) * source_height +
                        (source_height - y1);
                    for (int32_t y = 0; y < tile_height; ++y) {
                        destination[y] = tile[tile_height - y - 1][x];
                    }
                }
            }
        }
    }

    static void RotateDirectColumns(const uint16_t* source, uint16_t* output,
                                    int32_t source_width, int32_t source_height,
                                    int32_t first_column, int32_t last_column,
                                    bool byte_swapped) {
        alignas(16) uint16_t tile[kTileSize][kTileSize];
        for (int32_t y0 = 0; y0 < source_height; y0 += kTileSize) {
            const int32_t y1 = y0 + kTileSize < source_height
                                   ? y0 + kTileSize
                                   : source_height;
            for (int32_t x0 = first_column; x0 < last_column; x0 += kTileSize) {
                const int32_t x1 = x0 + kTileSize < last_column
                                       ? x0 + kTileSize
                                       : last_column;
                const int32_t tile_width = x1 - x0;
                const int32_t tile_height = y1 - y0;
                for (int32_t y = y0; y < y1; ++y) {
                    const uint16_t* pixel = source + y * source_width + x0;
                    uint16_t* tile_row = tile[y - y0];
                    for (int32_t x = 0; x < tile_width; ++x) {
                        const uint16_t value = *pixel++;
                        tile_row[x] = byte_swapped ? value : __builtin_bswap16(value);
                    }
                }
                for (int32_t x = 0; x < tile_width; ++x) {
                    uint16_t* destination =
                        output + (x0 + x) * source_height + (source_height - y1);
                    for (int32_t y = 0; y < tile_height; ++y) {
                        destination[y] = tile[tile_height - y - 1][x];
                    }
                }
            }
        }
    }

    static void RotateScaledDirectColumns(
        const uint16_t* source, uint16_t* output, int32_t source_width,
        int32_t source_height, int32_t first_column, int32_t last_column,
        uint8_t scale, bool byte_swapped) {
        (void)pxa_surface_transform_rgb565_270_columns(
            source, output, static_cast<uint16_t>(source_width),
            static_cast<uint16_t>(source_height),
            static_cast<uint16_t>(first_column),
            static_cast<uint16_t>(last_column), scale, byte_swapped);
    }

    static bool RotateDirectFrame(Context* context, const uint16_t* source,
                                  uint32_t source_width,
                                  uint32_t source_height, uint8_t scale,
                                  uint16_t* output, bool byte_swapped) {
        const int32_t split = scale == 1
                                  ? context->split_column
                                  : DirectSplit(static_cast<int32_t>(source_width));
        if (context->uses_worker) {
            context->source = source;
            context->job_output = output;
            context->source_is_direct = true;
            context->source_byte_swapped = byte_swapped;
            context->direct_source_width = static_cast<uint16_t>(source_width);
            context->direct_source_height = static_cast<uint16_t>(source_height);
            context->direct_scale = scale;
            xSemaphoreTake(context->worker_done, 0);
            xTaskNotifyGive(context->worker_task);
            if (scale == 1) {
                RotateDirectColumns(source, output,
                                    static_cast<int32_t>(source_width),
                                    static_cast<int32_t>(source_height), 0,
                                    split, byte_swapped);
            } else {
                RotateScaledDirectColumns(
                    source, output, static_cast<int32_t>(source_width),
                    static_cast<int32_t>(source_height), 0, split, scale,
                    byte_swapped);
            }
            if (xSemaphoreTake(context->worker_done, kWorkerDoneTimeout) == pdTRUE) {
                return true;
            }

            // The worker still owns the source and output. Wait for that
            // access to finish before falling back to a single-core retry.
            TaskHandle_t stalled_worker = context->worker_task;
            context->uses_worker = false;
            context->worker_task = nullptr;
            ESP_LOGE(kTag,
                     "Direct Surface rotation worker timed out after %u ms; "
                     "disabling parallel rotation",
                     static_cast<unsigned>(pdTICKS_TO_MS(kWorkerDoneTimeout)));
            if (stalled_worker != nullptr) {
                (void)xSemaphoreTake(context->worker_done, portMAX_DELAY);
                vTaskDelete(stalled_worker);
            }
        }
        if (scale == 1) {
            RotateDirectColumns(source, output,
                                static_cast<int32_t>(source_width),
                                static_cast<int32_t>(source_height), 0,
                                static_cast<int32_t>(source_width),
                                byte_swapped);
        } else {
            RotateScaledDirectColumns(
                source, output, static_cast<int32_t>(source_width),
                static_cast<int32_t>(source_height), 0,
                static_cast<int32_t>(source_width), scale, byte_swapped);
        }
        return true;
    }

    static void WorkerTask(void* arg) {
        auto* context = static_cast<Context*>(arg);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            const uint32_t started_us =
                static_cast<uint32_t>(esp_timer_get_time());
            if (context->source_is_direct) {
                const int32_t split =
                    context->direct_scale == 1
                        ? context->split_column
                        : DirectSplit(context->direct_source_width);
                if (context->direct_scale == 1) {
                    RotateDirectColumns(
                        context->source, context->job_output,
                        context->direct_source_width,
                        context->direct_source_height, split,
                        context->direct_source_width,
                        context->source_byte_swapped);
                } else {
                    RotateScaledDirectColumns(
                        context->source, context->job_output,
                        context->direct_source_width,
                        context->direct_source_height, split,
                        context->direct_source_width, context->direct_scale,
                        context->source_byte_swapped);
                }
            } else {
                RotateColumns(context->source, context->job_output,
                              context->logical_width, context->logical_height,
                              context->split_column, context->logical_width
#if CONFIG_PXA_ENABLED
                              , context->has_surface_frame
                                    ? &context->surface_frame : nullptr
#endif
                              );
            }
            context->worker_last_us =
                static_cast<uint32_t>(esp_timer_get_time()) - started_us;
            xSemaphoreGive(context->worker_done);
        }
    }

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
    static void UpdateAtomicMax(std::atomic<uint32_t>& maximum,
                                uint32_t value) {
        uint32_t current = maximum.load(std::memory_order_relaxed);
        while (value > current &&
               !maximum.compare_exchange_weak(current, value,
                                               std::memory_order_relaxed)) {
        }
    }
#endif

    static bool IRAM_ATTR ColorTransferDoneCallback(
        esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
        void* user_context) {
        auto* context = static_cast<Context*>(user_context);
        BaseType_t task_woken = pdFALSE;
        xSemaphoreGiveFromISR(context->transfer_done, &task_woken);
        return task_woken == pdTRUE;
    }

    static void SubmitTask(void* arg) {
        auto* context = static_cast<Context*>(arg);
        uint8_t buffer_index = 0;
        while (true) {
            xQueueReceive(context->ready_queue, &buffer_index, portMAX_DELAY);

            xSemaphoreTake(context->transfer_done, 0);
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
            const uint32_t send_started_us =
                static_cast<uint32_t>(esp_timer_get_time());
            context->submitted_frames.fetch_add(1, std::memory_order_relaxed);
#endif
            const esp_err_t result = esp_lcd_panel_draw_bitmap(
                context->panel, 0, 0, context->native_width,
                context->native_height, context->outputs[buffer_index]);

            if (result == ESP_OK) {
                if (xSemaphoreTake(context->transfer_done,
                                   kTransferDoneTimeout) == pdTRUE) {
                    const uint64_t completed_us =
                        static_cast<uint64_t>(esp_timer_get_time());
                    context->last_completed_frame_id.store(
                        context->output_frame_id[buffer_index],
                        std::memory_order_relaxed);
                    context->last_completed_timestamp_us.store(
                        completed_us, std::memory_order_relaxed);
                    context->last_completed_source.store(
                        context->output_source[buffer_index],
                        std::memory_order_relaxed);
                    context->last_completed_output.store(
                        static_cast<int8_t>(buffer_index),
                        std::memory_order_release);
                    context->completed_sequence.fetch_add(
                        1, std::memory_order_release);
#if CONFIG_PXA_ENABLED
                    if (context->output_source[buffer_index] ==
                            kFrameSourceSurface ||
                        context->output_source[buffer_index] ==
                            kFrameSourceComposed) {
                        context->pxa_visible_frames.fetch_add(
                            1, std::memory_order_relaxed);
                        if (context->pxa_visible_last_us != 0 &&
                            completed_us > context->pxa_visible_last_us) {
                            const uint64_t interval =
                                completed_us - context->pxa_visible_last_us;
                            const uint32_t interval_us =
                                interval > UINT32_MAX
                                    ? UINT32_MAX
                                    : static_cast<uint32_t>(interval);
                            uint32_t maximum =
                                context->pxa_visible_interval_max_us.load(
                                    std::memory_order_relaxed);
                            while (interval_us > maximum &&
                                   !context->pxa_visible_interval_max_us
                                        .compare_exchange_weak(
                                            maximum, interval_us,
                                            std::memory_order_relaxed)) {
                            }
                        }
                        context->pxa_visible_last_us = completed_us;
                        if (context->pxa_visible_window_started_us == 0) {
                            context->pxa_visible_window_started_us =
                                completed_us;
                            context->pxa_visible_window_frames = 0;
                        } else {
                            ++context->pxa_visible_window_frames;
                            const uint64_t window_us =
                                completed_us -
                                context->pxa_visible_window_started_us;
                            if (window_us >= UINT64_C(1000000)) {
                                const uint32_t fps_x10 =
                                    static_cast<uint32_t>(
                                        context->pxa_visible_window_frames *
                                        UINT64_C(10000000) / window_us);
                                uint32_t minimum =
                                    context->pxa_visible_min_1s_fps_x10.load(
                                        std::memory_order_relaxed);
                                while (fps_x10 < minimum &&
                                       !context
                                            ->pxa_visible_min_1s_fps_x10
                                            .compare_exchange_weak(
                                                minimum, fps_x10,
                                                std::memory_order_relaxed)) {
                                }
                                context->pxa_visible_window_started_us =
                                    completed_us;
                                context->pxa_visible_window_frames = 0;
                            }
                        }
                    }
                    pxa_esp_surface_note_frame_presented(
                        context->output_input_timestamp_us[buffer_index],
                        completed_us);
#endif
                    context->output_input_timestamp_us[buffer_index] = 0;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
                    context->completed_frames.fetch_add(
                        1, std::memory_order_relaxed);
#endif
                } else {
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
                    context->transfer_timeouts.fetch_add(
                        1, std::memory_order_relaxed);
#endif
                    ESP_LOGE(kTag, "Timed out waiting for LCD transfer completion");
                }
            } else {
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
                context->submit_errors.fetch_add(1, std::memory_order_relaxed);
#endif
                ESP_LOGE(kTag, "Panel submit failed: %s", esp_err_to_name(result));
                // A failed large transfer may still have earlier chunks in flight.
                vTaskDelay(kTransferErrorDrainDelay);
            }

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
            const uint32_t send_us =
                static_cast<uint32_t>(esp_timer_get_time()) - send_started_us;
            context->send_total_us.fetch_add(send_us, std::memory_order_relaxed);
            UpdateAtomicMax(context->send_max_us, send_us);
#endif
            xQueueSend(context->free_queue, &buffer_index, portMAX_DELAY);
        }
    }

    static bool AcquireOutputBuffer(Context* context, uint8_t* buffer_index) {
        if (xQueueReceive(context->free_queue, buffer_index, 0) == pdTRUE) {
            return true;
        }

        // One buffer is transmitting. Replace an older queued frame with the
        // newest logical frame instead of growing latency.
        if (xQueueReceive(context->ready_queue, buffer_index, 0) == pdTRUE) {
            ++context->dropped_queued_frames;
            return true;
        }

        ++context->dropped_no_buffer_frames;
        return false;
    }

    static void AdaptSplit(Context* context, uint32_t main_average_us,
                           uint32_t worker_average_us) {
        if (!context->uses_worker || context->frame_count == 0) {
            return;
        }

        int32_t next_split = context->split_column;
        if (main_average_us > worker_average_us + kAdaptThresholdUs) {
            next_split -= kTileSize;
        } else if (worker_average_us > main_average_us + kAdaptThresholdUs) {
            next_split += kTileSize;
        }
        context->split_column = AlignSplit(next_split, context->logical_width);
    }

    static void LogDiagnostics(Context* context, uint32_t now_us) {
        const uint32_t elapsed_us = now_us - context->report_started_us;
        if (elapsed_us < kReportIntervalUs) {
            return;
        }

        const uint32_t average_main_us = context->frame_count == 0 ? 0 :
            context->main_total_us / context->frame_count;
        const uint32_t average_worker_us = context->frame_count == 0 ? 0 :
            context->worker_total_us / context->frame_count;
        const uint32_t overlay_fps_x10 = elapsed_us == 0 ? 0 :
            static_cast<uint32_t>(context->frame_count * UINT64_C(10000000) /
                                  elapsed_us);
        const uint32_t overlay_rotate_us = context->frame_count == 0 ? 0 :
            context->rotate_total_us / context->frame_count;
        context->performance_fps_x10.store(overlay_fps_x10,
                                           std::memory_order_relaxed);
        context->performance_rotate_us.store(overlay_rotate_us,
                                              std::memory_order_relaxed);

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        const uint32_t fps_x10 = elapsed_us == 0 ? 0 : static_cast<uint32_t>(
            context->frame_count * 10ULL * 1000ULL * 1000ULL / elapsed_us);
        const uint32_t average_rotate_us = context->frame_count == 0 ? 0 :
            context->rotate_total_us / context->frame_count;
        const uint32_t submitted = context->submitted_frames.exchange(
            0, std::memory_order_relaxed);
        const uint32_t completed = context->completed_frames.exchange(
            0, std::memory_order_relaxed);
        const uint32_t submit_errors = context->submit_errors.exchange(
            0, std::memory_order_relaxed);
        const uint32_t transfer_timeouts = context->transfer_timeouts.exchange(
            0, std::memory_order_relaxed);
        const uint32_t send_total_us = context->send_total_us.exchange(
            0, std::memory_order_relaxed);
        const uint32_t send_max_us = context->send_max_us.exchange(
            0, std::memory_order_relaxed);
        const uint32_t average_send_us = submitted == 0 ? 0 :
            send_total_us / submitted;
        context->performance_send_us.store(average_send_us,
                                           std::memory_order_relaxed);
        const UBaseType_t free_buffers =
            uxQueueMessagesWaiting(context->free_queue);
        const UBaseType_t ready_buffers =
            uxQueueMessagesWaiting(context->ready_queue);
        const UBaseType_t worker_stack_words = context->worker_task == nullptr ?
            0 : uxTaskGetStackHighWaterMark(context->worker_task);
        const UBaseType_t submit_stack_words =
            uxTaskGetStackHighWaterMark(context->submit_task);
        const int32_t old_split = context->split_column;
#if CONFIG_PXA_ENABLED
        const uint32_t pxa_direct_fps_x10 = elapsed_us == 0 ? 0 :
            static_cast<uint32_t>(context->pxa_direct_frame_count * 10ULL *
                                  1000ULL * 1000ULL / elapsed_us);
        const uint32_t pxa_visible_frames =
            context->pxa_visible_frames.exchange(
                0, std::memory_order_relaxed);
        const uint32_t pxa_visible_fps_x10 = elapsed_us == 0 ? 0 :
            static_cast<uint32_t>(pxa_visible_frames * UINT64_C(10000000) /
                                  elapsed_us);
        uint32_t pxa_visible_min_1s_fps_x10 =
            context->pxa_visible_min_1s_fps_x10.exchange(
                UINT32_MAX, std::memory_order_relaxed);
        if (pxa_visible_min_1s_fps_x10 == UINT32_MAX)
            pxa_visible_min_1s_fps_x10 = 0;
        const uint32_t pxa_visible_interval_max_us =
            context->pxa_visible_interval_max_us.exchange(
                0, std::memory_order_relaxed);
        const uint32_t pxa_direct_rotate_us =
            context->pxa_direct_frame_count == 0 ? 0 :
            context->pxa_direct_rotate_total_us /
                context->pxa_direct_frame_count;
        pxa_esp_surface_input_metrics_t input_metrics = {};
        pxa_esp_surface_take_input_metrics(&input_metrics);
        const uint32_t sample_to_guest_us =
            input_metrics.sample_to_guest_count == 0 ? 0 :
            static_cast<uint32_t>(input_metrics.sample_to_guest_total_us /
                                  input_metrics.sample_to_guest_count);
        const uint32_t sample_to_present_us =
            input_metrics.sample_to_present_count == 0 ? 0 :
            static_cast<uint32_t>(input_metrics.sample_to_present_total_us /
                                  input_metrics.sample_to_present_count);
        const uint32_t sample_to_visible_us =
            input_metrics.sample_to_visible_count == 0 ? 0 :
            static_cast<uint32_t>(input_metrics.sample_to_visible_total_us /
                                  input_metrics.sample_to_visible_count);
#endif
#endif

        AdaptSplit(context, average_main_us, average_worker_us);
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        if (context->performance_log_enabled.load(std::memory_order_relaxed)) {
        ESP_LOGI(kTag,
                 "Parallel rotation: window=%" PRIu32 "ms frames=%" PRIu32
                 " (%" PRIu32 ".%u fps) areas=%" PRIu32
                 " rotate_us avg/max=%" PRIu32 "/%" PRIu32
                 " main/worker=%" PRIu32 "/%" PRIu32
                 " split=%ld->%ld drops=%" PRIu32 "/%" PRIu32,
                 elapsed_us / 1000, context->frame_count, fps_x10 / 10,
                 static_cast<unsigned>(fps_x10 % 10),
                 context->flush_area_count, average_rotate_us,
                 context->rotate_max_us, average_main_us, average_worker_us,
                 static_cast<long>(old_split),
                 static_cast<long>(context->split_column),
                 context->dropped_queued_frames,
                 context->dropped_no_buffer_frames);
        ESP_LOGI(kTag,
                 "Pipelined submit: submitted=%" PRIu32 " completed=%" PRIu32
                 " send_us avg/max=%" PRIu32 "/%" PRIu32
                 " submit_err=%" PRIu32 " timeout=%" PRIu32
                 " buffers_free/ready=%u/%u stack_words worker/submit=%u/%u",
                 submitted, completed, average_send_us, send_max_us,
                 submit_errors, transfer_timeouts,
                 static_cast<unsigned>(free_buffers),
                 static_cast<unsigned>(ready_buffers),
                 static_cast<unsigned>(worker_stack_words),
                 static_cast<unsigned>(submit_stack_words));
#if CONFIG_PXA_ENABLED
        ESP_LOGI(kTag,
                 "PXA direct: submitted=%" PRIu32 " (%" PRIu32 ".%u Hz) "
                 "visible=%" PRIu32 " (%" PRIu32 ".%u fps) "
                 "min_1s=%" PRIu32 ".%u fps interval_max=%" PRIu32 "us "
                 "rotate_us avg/max=%" PRIu32 "/%" PRIu32
                 " submit_failures=%" PRIu32 " composition_fallbacks=%" PRIu32
                 " active=%d",
                 context->pxa_direct_frame_count, pxa_direct_fps_x10 / 10,
                 static_cast<unsigned>(pxa_direct_fps_x10 % 10),
                 pxa_visible_frames, pxa_visible_fps_x10 / 10,
                 static_cast<unsigned>(pxa_visible_fps_x10 % 10),
                 pxa_visible_min_1s_fps_x10 / 10,
                 static_cast<unsigned>(pxa_visible_min_1s_fps_x10 % 10),
                 pxa_visible_interval_max_us,
                 pxa_direct_rotate_us, context->pxa_direct_rotate_max_us,
                 context->pxa_direct_submit_failures,
                 context->pxa_direct_composition_fallbacks,
                 static_cast<int>(context->pxa_direct_scanout_active));
        ESP_LOGI(kTag,
                 "PXA input latency us: guest avg/p95/max=%" PRIu32 "/%" PRIu32
                 "/%" PRIu32 " (%" PRIu32 ") present avg/p95/max=%" PRIu32
                 "/%" PRIu32 "/%" PRIu32 " (%" PRIu32
                 ") visible avg/p95/max=%" PRIu32 "/%" PRIu32 "/%" PRIu32
                 " (%" PRIu32 ")",
                 sample_to_guest_us,
                 input_metrics.sample_to_guest_p95_us,
                 input_metrics.sample_to_guest_max_us,
                 input_metrics.sample_to_guest_count,
                 sample_to_present_us,
                 input_metrics.sample_to_present_p95_us,
                 input_metrics.sample_to_present_max_us,
                 input_metrics.sample_to_present_count,
                 sample_to_visible_us,
                 input_metrics.sample_to_visible_p95_us,
                 input_metrics.sample_to_visible_max_us,
                 input_metrics.sample_to_visible_count);
#endif
        }
#endif

        context->report_started_us = now_us;
        context->frame_count = 0;
        context->flush_area_count = 0;
        context->rotate_total_us = 0;
        context->rotate_max_us = 0;
        context->main_total_us = 0;
        context->worker_total_us = 0;
        context->dropped_queued_frames = 0;
        context->dropped_no_buffer_frames = 0;
#if CONFIG_PXA_ENABLED
        context->pxa_direct_frame_count = 0;
        context->pxa_direct_rotate_total_us = 0;
        context->pxa_direct_rotate_max_us = 0;
        context->pxa_direct_submit_failures = 0;
        context->pxa_direct_composition_fallbacks = 0;
#endif
    }

    static void FlushCallback(lv_display_t* display, const lv_area_t* area,
                              uint8_t* color_map) {
        auto* context = GetContext();
        if (context == nullptr || context->display != display || area == nullptr ||
            color_map == nullptr) {
            if (display != nullptr) {
                lv_display_flush_ready(display);
            }
            return;
        }

#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        ++context->flush_area_count;
#endif
        if (!lv_display_flush_is_last(display)) {
            lv_display_flush_ready(display);
            return;
        }

        if (xSemaphoreTake(context->rotation_lock, portMAX_DELAY) != pdTRUE) {
            lv_display_flush_ready(display);
            return;
        }
        if (context->direct_scanout_active) {
            lv_display_flush_ready(display);
            xSemaphoreGive(context->rotation_lock);
            return;
        }

        uint8_t buffer_index = 0;
        if (!AcquireOutputBuffer(context, &buffer_index)) {
            if (context->dropped_no_buffer_frames == 1) {
                ESP_LOGW(kTag,
                         "No physical output buffer available; dropping frames");
            }
#if CONFIG_PXA_ENABLED
            if (pxa_esp_surface_has_pending_frame()) {
                (void)lv_async_call(InvalidateSurfaceArea, context);
            }
#endif
            lv_display_flush_ready(display);
            LogDiagnostics(context,
                           static_cast<uint32_t>(esp_timer_get_time()));
            xSemaphoreGive(context->rotation_lock);
            return;
        }

        const auto* source = reinterpret_cast<const uint16_t*>(color_map);
        uint16_t* output = context->outputs[buffer_index];
        context->source_is_direct = false;
#if CONFIG_PXA_ENABLED
        context->has_surface_frame =
            pxa_esp_surface_acquire_latest(&context->surface_frame);
#endif
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        const uint32_t rotation_started_us =
            static_cast<uint32_t>(esp_timer_get_time());
#endif
        uint32_t main_us = 0;
        uint32_t worker_us = 0;
        if (context->uses_worker) {
            context->source = source;
            context->job_output = output;
            context->worker_last_us = 0;
            xSemaphoreTake(context->worker_done, 0);
            xTaskNotifyGive(context->worker_task);
            const uint32_t main_started_us =
                static_cast<uint32_t>(esp_timer_get_time());
            RotateColumns(source, output, context->logical_width,
                          context->logical_height, 0, context->split_column
#if CONFIG_PXA_ENABLED
                          , context->has_surface_frame
                                ? &context->surface_frame : nullptr
#endif
                          );
            main_us = static_cast<uint32_t>(esp_timer_get_time()) -
                main_started_us;
            if (xSemaphoreTake(context->worker_done,
                               kWorkerDoneTimeout) != pdTRUE) {
                // The worker may still own both the LVGL source and this output
                // buffer. Do not reuse either until it acknowledges completion.
                // Deleting a task on the other core is asynchronous, so doing a
                // one-tick delay here allowed two rotations to write one output.
                TaskHandle_t stalled_worker = context->worker_task;
                context->uses_worker = false;
                context->worker_task = nullptr;
                ESP_LOGE(kTag,
                         "Rotation worker timed out after %u ms; disabling parallel "
                         "rotation and waiting before retrying output buffer %u",
                         static_cast<unsigned>(
                             pdTICKS_TO_MS(kWorkerDoneTimeout)),
                         static_cast<unsigned>(buffer_index));
                if (stalled_worker != nullptr) {
                    // RotateColumns has no blocking operations. Once it has
                    // signalled, it no longer accesses source or job_output.
                    (void)xSemaphoreTake(context->worker_done, portMAX_DELAY);
                    vTaskDelete(stalled_worker);
                }
                const uint32_t retry_started_us =
                    static_cast<uint32_t>(esp_timer_get_time());
                RotateColumns(source, output, context->logical_width,
                              context->logical_height, 0,
                              context->logical_width
#if CONFIG_PXA_ENABLED
                              , context->has_surface_frame
                                    ? &context->surface_frame : nullptr
#endif
                              );
                main_us += static_cast<uint32_t>(esp_timer_get_time()) -
                    retry_started_us;
                worker_us = 0;
            } else {
                worker_us = context->worker_last_us;
            }
        } else {
            const uint32_t main_started_us =
                static_cast<uint32_t>(esp_timer_get_time());
            RotateColumns(source, output, context->logical_width,
                          context->logical_height, 0, context->logical_width
#if CONFIG_PXA_ENABLED
                          , context->has_surface_frame
                                ? &context->surface_frame : nullptr
#endif
                          );
            main_us = static_cast<uint32_t>(esp_timer_get_time()) -
                main_started_us;
        }

        ++context->frame_count;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        const uint32_t rotate_us =
            static_cast<uint32_t>(esp_timer_get_time()) - rotation_started_us;
        context->rotate_total_us += rotate_us;
#endif
        context->main_total_us += main_us;
        context->worker_total_us += worker_us;
#if CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG
        if (rotate_us > context->rotate_max_us) {
            context->rotate_max_us = rotate_us;
        }
#endif

        CompositePerformanceOverlay(context, output);

        uint8_t output_source = kFrameSourceLvgl;
        uint64_t output_frame_id = 0;
        uint64_t output_input_timestamp_us = 0;
#if CONFIG_PXA_ENABLED
        if (context->has_surface_frame) {
            output_source = kFrameSourceComposed;
            output_frame_id = context->surface_frame.frame_id;
            output_input_timestamp_us =
                context->surface_frame.input_timestamp_us;
            pxa_esp_surface_release_frame(context->surface_frame.lease);
            context->has_surface_frame = false;
        }
#endif

        context->output_frame_id[buffer_index] = output_frame_id;
        context->output_source[buffer_index] = output_source;
        context->output_input_timestamp_us[buffer_index] =
            output_input_timestamp_us;

        if (xQueueSend(context->ready_queue, &buffer_index, 0) != pdTRUE) {
            ++context->dropped_no_buffer_frames;
            if (xQueueSend(context->free_queue, &buffer_index, 0) != pdTRUE) {
                ESP_LOGE(kTag,
                         "Unable to return output buffer %u after queue rejection; "
                         "quarantining it",
                         static_cast<unsigned>(buffer_index));
            }
        }

        // Rotation has copied the complete logical frame. LVGL may now render
        // the next frame while the submit task owns this physical buffer.
        lv_display_flush_ready(display);
        LogDiagnostics(context, static_cast<uint32_t>(esp_timer_get_time()));
        xSemaphoreGive(context->rotation_lock);
    }

#if CONFIG_PXA_ENABLED
    static void InvalidateSurfaceArea(void* opaque) {
        auto* context = static_cast<Context*>(opaque);
        if (context == nullptr || context != GetContext() ||
            context->display == nullptr) {
            return;
        }
        lv_lock();
        lv_obj_t* screen = lv_display_get_screen_active(context->display);
        if (screen != nullptr) {
            lv_area_t trigger = {0, 0, 0, 0};
            lv_obj_invalidate_area(screen, &trigger);
        }
        lv_unlock();
    }

    static bool PxaFrameCanScanoutDirectly(
        const Context* context, const pxa_esp_surface_frame_t& frame) {
        const auto& overlay = frame.ui_alpha_plane;
        const uint8_t scale = DirectFrameScale(context, frame.width,
                                               frame.height);
        return frame.visible &&
               frame.format == PXA_SURFACE_FORMAT_RGB565 &&
               (frame.flags & PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT) != 0 &&
               frame.x == 0 && frame.y == 0 &&
               scale != 0 &&
               frame.stride_bytes ==
                   static_cast<uint32_t>(frame.width) * sizeof(uint16_t) &&
               frame.opaque_ui_region_count == 0 &&
               !pxa_esp_surface_composition_required() &&
               (!overlay.visible || overlay.opacity == 0 ||
                overlay.pixels == nullptr || overlay.alpha == nullptr);
    }

    static void EndPxaDirectScanout(Context* context) {
        if (!context->pxa_direct_scanout_active) return;
        context->pxa_direct_scanout_active = false;
        if (!EndDirectScanout(kPxaDirectTransitionTimeout) ||
            !WaitForPendingTransfers(kPxaDirectTransitionTimeout)) {
            ESP_LOGW(kTag, "Timed out returning PXA Surface to LVGL composition");
        }
    }

    static void PxaDirectFrameCopied(void* opaque, uint8_t) {
        auto* context = static_cast<Context*>(opaque);
        if (context == nullptr || context != GetContext() ||
            context->pxa_direct_lease == 0) {
            return;
        }
        const uint64_t lease = context->pxa_direct_lease;
        context->pxa_direct_lease = 0;
        pxa_esp_surface_release_frame(lease);
    }

    static void PresentPxaSurface(Context* context) {
        pxa_esp_surface_frame_t frame = {};
        // While a trusted system overlay is visible, the LVGL flush path is
        // the sole consumer of the Surface mailbox. Acquiring here and then
        // releasing a GuestMapped frame would return its buffer to the Guest
        // before LVGL had a chance to composite it behind the overlay.
        if (pxa_esp_surface_composition_required()) {
            ++context->pxa_direct_composition_fallbacks;
            EndPxaDirectScanout(context);
            InvalidateSurfaceArea(context);
            return;
        }
        if (!pxa_esp_surface_acquire_latest_for_direct(&frame)) {
            EndPxaDirectScanout(context);
            InvalidateSurfaceArea(context);
            return;
        }

        if (!PxaFrameCanScanoutDirectly(context, frame)) {
            ++context->pxa_direct_composition_fallbacks;
            pxa_esp_surface_release_frame(frame.lease);
            EndPxaDirectScanout(context);
            InvalidateSurfaceArea(context);
            return;
        }

        if (!context->pxa_direct_scanout_active) {
            if (!BeginDirectScanout(kPxaDirectTransitionTimeout)) {
                pxa_esp_surface_release_frame(frame.lease);
                InvalidateSurfaceArea(context);
                return;
            }
            context->pxa_direct_scanout_active = true;
        }

        context->pxa_direct_lease = frame.lease;
        if (SubmitDirectFrame(reinterpret_cast<const uint16_t*>(frame.pixels),
                              frame.width, frame.height, false, 0,
                              frame.frame_id,
                              frame.input_timestamp_us,
                              PxaDirectFrameCopied, context)) {
            return;
        }
        context->pxa_direct_lease = 0;
        pxa_esp_surface_release_frame(frame.lease);
        EndPxaDirectScanout(context);
        InvalidateSurfaceArea(context);
    }

    static void SurfaceFrameReady(void* opaque) {
        auto* context = static_cast<Context*>(opaque);
        if (context == nullptr || context != GetContext() ||
            context->pxa_presenter_task == nullptr) {
            return;
        }
        xTaskNotifyGive(context->pxa_presenter_task);
    }

    static void PxaPresenterTask(void* opaque) {
        auto* context = static_cast<Context*>(opaque);
        for (;;) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (context == nullptr || context != GetContext() ||
                context->display == nullptr) {
                continue;
            }
            PresentPxaSurface(context);
        }
    }
#endif
};

}  // namespace zuowei_pai_touch
