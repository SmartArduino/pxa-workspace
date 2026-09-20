idf_component_register(
    SRCS
        "src/esp32s31_korvo_1_board_port.cc"
        "src/esp32s31_korvo_1_adc_calibration.cc"
        "src/esp32s31_korvo_1_hardware.cc"
        "src/esp32s31_korvo_1_pxa_surface.cc"
        "src/esp32s31_korvo_1_audio.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ppa
        esp_event
        esp_lcd
        esp_timer
        espressif__esp_codec_dev
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_gt1151
        espressif__esp_lv_decoder
        espressif__esp_lvgl_port
        espressif__button
        joltwallet__littlefs
        lvgl
        pxa
        pxa_board_api
        pxa_system_reference_ui
        pxa_system_standard)

# esp_lv_decoder includes LVGL's private PPA declarations when LV_USE_PPA is
# enabled, so its standalone decoder target also needs the PPA include path.
if(CONFIG_LV_USE_PPA AND TARGET lvgl_docoder_lib)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::esp_driver_ppa)
endif()

# PNG icons do not use the JPEG engine; reserving it at decoder init leaves
# too little internal SRAM for a running PXA application on this board.
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
