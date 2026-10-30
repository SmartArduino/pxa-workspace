idf_component_register(
    SRCS "src/doit_gamekit_1_board.cc" "src/doit_gamekit_1_audio.cc"
         "src/doit_gamekit_1_panel.cc" "src/doit_gamekit_1_touch.cc"
         "src/doit_gamekit_1_battery.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect esp_adc esp_driver_gpio esp_driver_spi esp_driver_sdmmc
        esp_driver_i2s esp_driver_i2c esp_driver_ledc esp_lcd esp_netif esp_timer fatfs sdmmc esp_partition joltwallet__littlefs
        espressif__esp_lv_decoder espressif__esp_lvgl_port
        lvgl pxa pxadb pxa_board_api pxa_board_common pxa_system_reference_ui
        pxa_system_standard)

if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
endif()
if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${lvgl_lib} PRIVATE idf::espressif__freetype)
    if(TARGET lvgl_docoder_lib)
        target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
    endif()
endif()
