set(PXADB_PRIV_REQUIRES
    esp_app_format
    esp_driver_usb_serial_jtag
    esp_hw_support
    esp_psram
    esp_system
    esp_timer
    freertos
    log
    mbedtls
    pxa)

idf_component_register(
    SRCS "src_pxa_service.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${PXADB_PRIV_REQUIRES})
