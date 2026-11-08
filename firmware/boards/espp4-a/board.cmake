set(PXA_BOARD_TARGET "esp32p4")
set(PXA_BOARD_COMPONENT "espp4-a")
set(PXA_BOARD_ENABLE_PERIPHERALS ON)
set(PXA_BOARD_BOOTLOADER_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/bootloader_components")
# The module's SDIO radio is not an ESP-Hosted slave. Keep its unmodified
# supplier component private to this board's dependency graph.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/vendor/p4_wireless")
