if(CONFIG_PXADB_ENABLED AND (CONFIG_LIBC_NEWLIB_NANO_FORMAT OR CONFIG_NEWLIB_NANO_FORMAT))
    message(FATAL_ERROR "PXADB requires 64-bit printf support; disable nano printf to preserve timestamps and screenshot metadata.")
endif()

set(PXADB_PRIV_REQUIRES
    esp_app_format
    esp_driver_gpio
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_hw_support
    esp_new_jpeg
    esp_psram
    esp_system
    esp_timer
    freertos
    log
    joltwallet__littlefs
    mbedtls
    pxa)

idf_component_register(
    SRCS "src_pxa_service.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${PXADB_PRIV_REQUIRES})

# Product main components resolve their board after dependencies have been
# registered. Read the final build property at generation time.
idf_build_get_property(PXADB_BOARD_ID PXA_BOARD_ID GENERATOR_EXPRESSION)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_FIRMWARE_BOARD="${PXADB_BOARD_ID}")

if(CONFIG_PXADB_TRANSPORT_USB_CDC)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__esp_tinyusb)
endif()
