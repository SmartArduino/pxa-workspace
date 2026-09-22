#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

namespace korvo_pxa_surface {

using DirectScanoutTransitionCallback = bool (*)(
    bool entering, void* next_buffer, void* displayed_buffer, void* context);

struct CompletedFrameInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride_bytes = 0;
    uint64_t frame_id = 0;
    uint64_t completed_timestamp_us = 0;
    const char* source = "unknown";
};

bool Install(lv_display_t* display, esp_lcd_panel_handle_t panel,
             SemaphoreHandle_t frame_done_semaphore,
             DirectScanoutTransitionCallback transition_callback,
             void* transition_context);
void ComposeFrame(uint8_t* pixels);
bool DirectScanoutActive();

/* Copies the newest completed panel frame as RGB565LE rows. While direct
 * scanout is active the triple buffer keeps both the queued and the scanned
 * buffer fully composed, so the capture never reads a buffer being written.
 * after_present waits for a queued switch to complete first (bounded by
 * timeout_ms) so the caller sees a frame that reached the panel after the
 * request. Falls back to the active LVGL buffer when direct scanout is off. */
bool SnapshotDisplayedFrame(uint16_t* pixels, size_t pixel_count,
                            bool after_present, uint32_t timeout_ms,
                            CompletedFrameInfo* info);

}  // namespace korvo_pxa_surface
