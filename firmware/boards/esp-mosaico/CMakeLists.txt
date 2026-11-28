idf_build_get_property(use_esp_openh264 PXA_USE_ESP_OPENH264)
if(use_esp_openh264)
    set(video_dependency pxa_video_codecs)
endif()
idf_component_register(
    SRCS
        "src/esp_mosaico_board_port.cc"
        "src/esp_mosaico_diagnostics.cc"
        "src/esp_mosaico_hardware.cc"
        "src/esp_mosaico_usb_restart.cc"
        "src/esp_mosaico_audio.cc"
        "src/esp_mosaico_storage.cc"
        "src/esp_mosaico_sensors.cc"
        "src/esp_mosaico_haptics.cc"
        "src/esp_mosaico_peripherals.cc"
        "vendor/bmi270/bmi2.c"
        "vendor/bmi270/bmi270.c"
        "vendor/bmm150/bmm150.c"
        "src/esp_mosaico_pxa_surface.cc"
        "src/esp_mosaico_video_copy.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/bmi270" "vendor/bmm150"
    PRIV_REQUIRES
        ${video_dependency}
        78__esp-wifi-connect
        78__esp_lcd_touch_cst92xx
        efuse
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_ppa
        esp_driver_dma
        esp_driver_spi
        esp_driver_uart
        esp_blockdev
        fatfs
        espressif__spi_nand_flash
        joltwallet__littlefs
        esp_lcd
        esp_netif
        esp_timer
        esp_hal_usb
        esp_hw_support
        esp_rom
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
        pxa_board_common
        pxa_lvgl_memory
        pxa_system_reference_ui
        pxa_system_standard
        pxadb)
idf_component_get_property(mosaico_video_host pxa COMPONENT_LIB)
set_property(SOURCE "${PXSYS_ROOT}/libpxa/src/services/video/video_service.c"
    TARGET_DIRECTORY ${mosaico_video_host} APPEND PROPERTY COMPILE_DEFINITIONS
    PXA_VIDEO_COPY_ROWS=mosaico_video_copy_rows)
if(use_esp_openh264)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_USE_ESP_OPENH264=1)
endif()

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_cpu_reset" "-Wl,--undefined=__wrap_esp_cpu_reset"
    "-Wl,--undefined=mosaico_video_copy_rows")

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
