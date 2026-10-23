# Vendor only the ESP32-S3 LCD output and the supplied 40-pin panel waveform.
set(epd_src vendor/epdiy)
idf_component_register(
    SRCS src/epdiy_v7_board.cc src/epdiy_surface.cc
        ${epd_src}/epdiy.c ${epd_src}/render.c ${epd_src}/font.c
        ${epd_src}/diff.S ${epd_src}/highlevel.c ${epd_src}/panel.c
        ${epd_src}/output_lcd/render_lcd.c ${epd_src}/output_lcd/lcd_driver.c
        ${epd_src}/output_common/lut.c ${epd_src}/output_common/lut.S
        ${epd_src}/output_common/line_queue.c ${epd_src}/output_common/render_context.c
        ${epd_src}/output_common/render_method.c
        ${epd_src}/board/epd_board.c ${epd_src}/board/epd_board_v7.c
        ${epd_src}/board/pca9555.c ${epd_src}/board/tps65185.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS ${epd_src} ${epd_src}/board
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_rmt
        esp_driver_ledc esp_lcd esp_timer esp_netif
        78__esp-wifi-connect espressif__esp_lv_decoder espressif__freetype
        lvgl pxa pxadb pxa_board_api pxa_board_common pxa_lvgl_memory
        pxa_system_standard pxa_system_reference_ui)

# Upstream's C formatting predates IDF's int32_t format checks.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-format>)
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC idf::pxa_lvgl_memory PRIVATE idf::espressif__freetype)
if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
endif()
