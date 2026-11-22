idf_component_register(SRCS "pxa_integration.cc"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_timer joltwallet__littlefs lvgl nvs_flash pxa
             pxa_board_api pxa_system_esp pxa_system_reference_ui
             pxa_system_renderer_lvgl pxa_system_standard
             espressif__esp_lvgl_port pxadb 78__esp-wifi-connect
    PRIV_REQUIRES pxa_lvgl_input esp_hw_support esp_partition)

idf_build_get_property(pxa_integration_board PXA_BOARD_ID)
if(pxa_integration_board STREQUAL "pai-touch")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_START_RUNTIME_BEFORE_PXADB=1)
endif()
