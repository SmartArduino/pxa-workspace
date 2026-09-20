idf_component_register(SRCS "pxa_board_api.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log lvgl nvs_flash pxa_system_core pxa_system_reference_ui pxa_system_standard)
