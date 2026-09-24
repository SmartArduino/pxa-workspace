idf_component_register(SRCS "pxa_integration.cc"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_timer joltwallet__littlefs lvgl nvs_flash pxa
             pxa_board_api pxa_system_esp pxa_system_reference_ui
             pxa_system_renderer_lvgl pxa_system_standard
             espressif__esp_lvgl_port pxadb 78__esp-wifi-connect)
