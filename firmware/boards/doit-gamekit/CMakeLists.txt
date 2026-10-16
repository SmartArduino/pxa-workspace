idf_component_register(
    SRCS "src/doit_gamekit_board.cc" "src/doit_gamekit_audio.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect esp_driver_gpio esp_driver_spi esp_driver_sdspi
        esp_driver_i2s esp_driver_ledc esp_lcd esp_netif esp_timer fatfs sdmmc
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
