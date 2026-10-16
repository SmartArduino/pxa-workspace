#pragma once
#include <driver/spi_master.h>
struct esp_lcd_panel_io_t;
using esp_lcd_panel_io_handle_t = esp_lcd_panel_io_t*;
using esp_lcd_panel_io_color_trans_done_cb_t = bool (*)(esp_lcd_panel_io_t*, void*, void*);
struct esp_lcd_panel_io_callbacks_t { esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done{}; };
struct esp_lcd_panel_io_spi_config_t {
    int dc_gpio_num{-1}, cs_gpio_num{}, spi_mode{}, lcd_cmd_bits{32}, lcd_param_bits{8};
    unsigned pclk_hz{}; size_t trans_queue_depth{2};
    int cs_ena_pretrans{}, cs_ena_posttrans{};
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done{};
    void* user_ctx{};
    struct { unsigned quad_mode:1, octal_mode:1, sio_mode:1, lsb_first:1, cs_high_active:1; } flags{};
};
