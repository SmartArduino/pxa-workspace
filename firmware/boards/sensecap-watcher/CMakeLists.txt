idf_component_register(
    SRCS
        "src/sensecap_watcher_board_port.cc"
        "src/watcher_hardware.cc"
        "src/watcher_io_expander.cc"
        "src/watcher_battery.cc"
        "src/watcher_audio.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_spi
        esp_event
        esp_lcd
        esp_timer
        espressif__button
        espressif__esp_codec_dev
        espressif__esp_lcd_spd2010
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_spd2010
        espressif__esp_lvgl_port
        espressif__knob
        espressif__led_strip
        lvgl
        pxa
        pxa_board_api
        pxa_system_reference_ui
        pxa_system_standard)

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
endif()
