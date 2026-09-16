#pragma once

#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <cstdint>

// Wraps an SPI panel so full-screen updates start on the panel's TE edge.
esp_err_t CreateTeSynchronizedPanel(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_handle_t panel,
                                    gpio_num_t te_gpio,
                                    int width,
                                    int height,
                                    uint32_t update_interval_te_edges,
                                    esp_lcd_panel_handle_t* out_panel);
