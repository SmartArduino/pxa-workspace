idf_component_register(
    SRCS
        "src/pai_touch_board_port.cc"
        "src/pai_touch_hardware.cc"
        "src/rpc701_audio.cc"
        "src/adc_battery_monitor.cc"
        "src/button.cc"
        "src/te_sync_panel.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_driver_uart
        esp_event
        esp_lcd
        esp_timer
        espressif__adc_battery_estimation
        espressif__button
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        espressif__gmf_audio
        esp_lcd_touch_cst826
        joltwallet__littlefs
        lvgl
        nvs_flash
        pxa
        pxadb
        pxa_board_api
        pxa_system_reference_ui
        pxa_system_standard
        rpc_701
        rpc_adapter)

# This board owns the direct scanout presenter. It is not part of the generic
# port API and remains selected only for the pai-touch build profile.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG=1)

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
endif()
