# Board metadata read before ESP-IDF discovers components.
set(PXA_BOARD_TARGET "esp32s3")
# ESP-IDF component exporting pxa_board.h for this board profile.
set(PXA_BOARD_COMPONENT "pai-touch")
set(PXA_BOARD_ENABLE_PERIPHERALS ON)
if(NOT DEFINED PXA_ENABLE_VOICE_TRIGGER)
    set(PXA_ENABLE_VOICE_TRIGGER ON)
endif()
