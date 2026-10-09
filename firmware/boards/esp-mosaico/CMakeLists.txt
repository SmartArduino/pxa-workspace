idf_component_register(
    SRCS
        "src/esp_mosaico_board_port.cc"
        "src/esp_mosaico_diagnostics.cc"
        "src/esp_mosaico_hardware.cc"
        "src/esp_mosaico_audio.cc"
        "src/esp_mosaico_pxa_surface.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect
        78__esp_lcd_touch_cst92xx
        efuse
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_ppa
        esp_driver_spi
        esp_lcd
        esp_netif
        esp_timer
        espressif__button
        espressif__esp_codec_dev
        espressif__esp_lcd_co5300
        espressif__esp_lcd_touch
        espressif__esp_lv_decoder
        espressif__esp_lvgl_adapter
        espressif__esp_tinyusb
        lvgl
        pxa
        pxa_board_api
        pxa_lvgl_memory
        pxa_system_reference_ui
        pxa_system_standard
        pxadb)

if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
endif()

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
    if(TARGET lvgl_docoder_lib)
        target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
    endif()
endif()

# Resolve the custom PSRAM allocator after LVGL's archive, as on Watcher.
if(CONFIG_LV_USE_CUSTOM_MALLOC)
    idf_component_get_property(MOSAICO_LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${MOSAICO_LVGL_LIB} PUBLIC idf::pxa_lvgl_memory)
endif()
