#pragma once

#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

namespace korvo_pxa_surface {

bool Install(lv_display_t* display, esp_lcd_panel_handle_t panel,
             SemaphoreHandle_t vsync_semaphore);
void ComposeFrame(uint8_t* pixels);
bool DirectScanoutActive();

}  // namespace korvo_pxa_surface
