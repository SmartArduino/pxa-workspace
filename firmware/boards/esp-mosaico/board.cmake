set(PXA_BOARD_TARGET "esp32s31")
set(PXA_BOARD_COMPONENT "esp-mosaico")
set(PXA_BOARD_ENABLE_PERIPHERALS ON)

# ESP-IDF uses this hook for both image generation and flash targets.
if(NOT DEFINED ENV{ESPTOOL_WRAPPER} OR "$ENV{ESPTOOL_WRAPPER}" STREQUAL "")
    set(ENV{ESPTOOL_WRAPPER} "${CMAKE_CURRENT_LIST_DIR}/../../../tools/mosaico_esptool.py")
endif()
