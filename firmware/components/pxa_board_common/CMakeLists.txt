# Shared board-side helpers. Everything a second board can reuse belongs here;
# panel bring-up, presenters and DMA hot paths stay in the board component.
idf_component_register(
    SRCS
        "src/display_profile.cc"
        "src/ledc_backlight.cc"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc pxa_board_api pxa_system_core)
