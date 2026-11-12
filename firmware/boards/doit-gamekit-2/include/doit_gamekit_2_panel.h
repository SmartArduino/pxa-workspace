#pragma once
#include <cstdint>
#include <esp_lcd_panel_io.h>

class DoitGamekit2Panel {
public:
    bool Initialize(esp_lcd_panel_io_handle_t io);
    esp_err_t DisplayOn();
    esp_err_t Draw(const uint16_t* native);
private:
    esp_lcd_panel_io_handle_t io_ = nullptr;
};
