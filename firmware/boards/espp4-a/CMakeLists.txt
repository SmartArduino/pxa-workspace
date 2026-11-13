idf_component_register(
    LDFRAGMENTS camera.lf
    SRCS src/espp4_a_board.cc src/espp4_a_audio.cc src/espp4_a_panel.c src/espp4_a_surface.cc src/espp4_a_rotation.cc
        src/espp4_a_sdio.cc src/espp4_a_sd.cc src/espp4_a_camera.cc src/espp4_a_images.cc src/espp4_a_usb.cc src/espp4_a_usb_restart.cc vendor/gc4023/gc4023.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS vendor/gc4023/include vendor/gc4023
        "${PXSYS_ROOT}/libpxa/adapters/include"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s pxa_render_accel
        esp_driver_cam esp_driver_isp esp_driver_jpeg esp_driver_ppa
        espressif__esp_cam_sensor espressif__esp_sccb_intf espressif__esp_new_jpeg
        esp_driver_ledc esp_driver_sdmmc esp_driver_uart esp_lcd esp_hw_support esp_timer
        esp_netif esp_wifi fatfs sdmmc p4_wireless esp_hal_usb espressif__esp_tinyusb
        78__esp-wifi-connect espressif__esp_lcd_touch_st7123
        espressif__esp_codec_dev espressif__esp_lv_decoder espressif__freetype
        pxa pxadb pxa_board_api pxa_board_common pxa_lvgl_memory
        pxa_system_standard pxa_system_reference_ui)

target_sources(${COMPONENT_LIB} PRIVATE src/espp4_a_pixels.cc src/espp4_a_pixels.S)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_cpu_reset" "-Wl,--undefined=__wrap_esp_cpu_reset")
option(PXA_ESPP4_PIXEL_ACCEL "Use P4 v3 PIE RGB565 pixel primitives" ON)
option(PXA_ESPP4_NATIVE_UI_DIRECT "Redraw only damaged native LVGL regions between guest frames" ON)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_ESPP4_NATIVE_UI_DIRECT=$<BOOL:${PXA_ESPP4_NATIVE_UI_DIRECT}>)
option(PXA_ESPP4_SD_SINGLE_BLOCK "Diagnostic single-block TF transfers" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_SD_SINGLE_BLOCK=$<BOOL:${PXA_ESPP4_SD_SINGLE_BLOCK}>)
option(PXA_ESPP4_SD_SELFTEST "Run an owned temporary-file TF throughput/integrity test at boot" OFF)
if(PXA_ESPP4_SD_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE tests/sd_device_test.cc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_SD_SELFTEST=1)
endif()
option(PXA_ESPP4_PIXEL_SELFTEST "Include P4 pixel differential tests" OFF)
option(PXA_ESPP4_PPA_FILL "Use PPA for cache-line-aligned bulk clears (benchmark first)" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_PIXEL_ACCEL=$<BOOL:${PXA_ESPP4_PIXEL_ACCEL}>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_PPA_FILL=$<BOOL:${PXA_ESPP4_PPA_FILL}>)
if(PXA_ESPP4_PIXEL_ACCEL)
    idf_component_get_property(espp4_pixel_lvgl lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${espp4_pixel_lvgl} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_definitions(${espp4_pixel_lvgl} PUBLIC
        LV_USE_DRAW_SW_ASM=255 LV_DRAW_SW_ASM_CUSTOM_INCLUDE="espp4_a_pixels_lvgl.h")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,espp4_a_lvgl_fill")
    idf_component_get_property(espp4_pixel_core pxa COMPONENT_LIB)
    set_property(SOURCE "${PXSYS_ROOT}/libpxa/src/services/game_render/raster.c"
        "${PXSYS_ROOT}/libpxa/src/services/game_render/raster_rgb565.c"
        TARGET_DIRECTORY ${espp4_pixel_core} APPEND PROPERTY COMPILE_DEFINITIONS
        PXA_RASTER_FILL_RGB565=espp4_a_fill_rgb565)
endif()
if(PXA_ESPP4_PIXEL_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE tests/pixels_device_test.cc)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_PIXEL_SELFTEST=1)
endif()

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
foreach(sdio_operation read_bytes write_bytes read_blocks write_blocks read_byte write_byte)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_io_${sdio_operation}"
        "-Wl,--undefined=__wrap_sdmmc_io_${sdio_operation}")
endforeach()
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_card_init" "-Wl,--undefined=__wrap_sdmmc_card_init")
foreach(sd_operation read_sectors write_sectors)
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=sdmmc_${sd_operation}"
        "-Wl,--undefined=__wrap_sdmmc_${sd_operation}")
endforeach()
idf_component_get_property(espp4_lvgl lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${espp4_lvgl} PUBLIC idf::pxa_lvgl_memory PRIVATE idf::espressif__freetype)
if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
endif()
