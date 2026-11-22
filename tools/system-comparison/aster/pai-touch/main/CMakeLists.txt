idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
  REQUIRES aster_apps aster_connectivity aster_runtime aster_render aster_transfer
  esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_ledc
  esp_driver_usb_serial_jtag esp_psram fatfs wear_levelling)
