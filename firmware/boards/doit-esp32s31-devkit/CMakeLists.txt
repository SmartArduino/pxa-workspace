idf_component_register(
    SRCS src/doit_s31_board.cc src/doit_s31_audio.cc src/doit_s31_panel.c src/doit_s31_surface.cc

    INCLUDE_DIRS include
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s
        esp_driver_ledc esp_driver_sdmmc esp_driver_uart esp_lcd esp_hw_support esp_timer
        esp_netif esp_wifi fatfs sdmmc
        78__esp-wifi-connect
        espressif__esp_codec_dev espressif__esp_lv_decoder espressif__freetype
        pxa pxadb pxa_board_api pxa_board_common pxa_lvgl_memory
        pxa_system_standard pxa_system_reference_ui)

idf_component_get_property(doit_s31_lvgl lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${doit_s31_lvgl} PUBLIC idf::pxa_lvgl_memory PRIVATE idf::espressif__freetype)
if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
endif()

option(PXA_DOIT_S31_PANEL_DIAGNOSTICS "Enable board-local live panel comparisons through PXADB test keys" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_DOIT_S31_PANEL_DIAGNOSTICS=$<BOOL:${PXA_DOIT_S31_PANEL_DIAGNOSTICS}>)
