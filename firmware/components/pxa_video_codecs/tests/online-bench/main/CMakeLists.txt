set(board "${CMAKE_CURRENT_LIST_DIR}/../../../../../boards/esp-mosaico")
idf_component_register(
    SRCS "main.cpp" "board_io.cpp" "${board}/src/esp_mosaico_usb_restart.cc"
    PRIV_INCLUDE_DIRS "${board}/include" "${BILIBILI_DIR}"
    PRIV_REQUIRES esp_openh264 esp_tinyusb esp_timer esp_hw_support esp_hal_usb
        esp_wifi esp_netif esp_http_client mbedtls nvs_flash
        esp_audio_codec esp_codec_dev efuse esp_driver_gpio esp_driver_i2c
        esp_driver_spi esp_driver_i2s esp_lcd esp_lcd_co5300)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_cpu_reset" "-Wl,--undefined=__wrap_esp_cpu_reset")
