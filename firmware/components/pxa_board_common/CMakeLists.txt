# Shared board-side helpers. Everything a second board can reuse belongs here;
# panel bring-up, presenters and DMA hot paths stay in the board component.
idf_build_get_property(_board_peripherals PXA_ENABLE_PERIPHERALS)
set(_board_extra_sources)
set(_board_extra_dependencies)
if(_board_peripherals)
    list(APPEND _board_extra_sources "rgb_port.cc")
    list(APPEND _board_extra_sources "peripheral_backends.cc")
    list(APPEND _board_extra_sources "i2c_port.cc")
    list(APPEND _board_extra_sources "vision_port.cc")
    list(APPEND _board_extra_dependencies pxa esp_driver_i2c)
endif()
idf_component_register(
    SRCS
        "src/boot_screen.cc"
        "src/display_profile.cc"
        "src/ledc_backlight.cc"
        ${_board_extra_sources}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc lvgl pxa_board_api pxa_system_core
    PRIV_REQUIRES ${_board_extra_dependencies})
