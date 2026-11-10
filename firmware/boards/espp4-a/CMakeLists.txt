idf_component_register(
    LDFRAGMENTS camera.lf
    SRCS src/espp4_a_board.cc src/espp4_a_audio.cc src/espp4_a_panel.c src/espp4_a_surface.cc src/espp4_a_rotation.cc
        src/espp4_a_sdio.cc src/espp4_a_camera.cc src/espp4_a_images.cc vendor/gc4023/gc4023.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS vendor/gc4023/include vendor/gc4023
        "${PXSYS_ROOT}/libpxa/adapters/include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s
        esp_driver_cam esp_driver_isp esp_driver_jpeg esp_driver_ppa
        espressif__esp_cam_sensor espressif__esp_sccb_intf espressif__esp_new_jpeg
        esp_driver_ledc esp_driver_sdmmc esp_driver_uart esp_lcd esp_hw_support esp_timer
        esp_netif esp_wifi fatfs sdmmc p4_wireless
        78__esp-wifi-connect espressif__esp_lcd_touch_st7123
        espressif__esp_codec_dev espressif__esp_lv_decoder espressif__freetype
        pxa pxadb pxa_board_api pxa_board_common pxa_lvgl_memory
        pxa_system_standard pxa_system_reference_ui)

option(PXA_ESPP4_ROTATION_SELFTEST "Include the on-device RGB565 rotation differential test" OFF)
if(PXA_ESPP4_ROTATION_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE tests/rotation_device_test.cc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_ROTATION_SELFTEST=1)
endif()

option(PXA_ESPP4_AUDIO_SELFTEST "Play a bounded tone and verify the ES8311 DAC reference at boot" OFF)
if(PXA_ESPP4_AUDIO_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE tests/audio_device_test.cc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_AUDIO_SELFTEST=1)
endif()

# The shared provisioning UI requests APSTA. This module supports AP or STA;
# its board-local wrapper selects AP while provisioning (STA is stopped first).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_set_mode")
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
foreach(sdio_operation read_bytes write_bytes read_blocks write_blocks)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_io_${sdio_operation}"
        "-Wl,--undefined=__wrap_sdmmc_io_${sdio_operation}")
endforeach()
idf_component_get_property(espp4_lvgl lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${espp4_lvgl} PUBLIC idf::pxa_lvgl_memory PRIVATE idf::espressif__freetype)
if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
endif()
