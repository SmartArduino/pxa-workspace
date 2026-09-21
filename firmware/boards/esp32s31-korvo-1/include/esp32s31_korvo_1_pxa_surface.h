#pragma once

#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

namespace korvo_pxa_surface {

using DirectScanoutTransitionCallback = bool (*)(
    bool entering, void* next_buffer, void* displayed_buffer, void* context);

bool Install(lv_display_t* display, esp_lcd_panel_handle_t panel,
             SemaphoreHandle_t vsync_semaphore,
             DirectScanoutTransitionCallback transition_callback,
             void* transition_context);
void ComposeFrame(uint8_t* pixels);
bool DirectScanoutActive();

}  // namespace korvo_pxa_surface
