idf_component_register(
    SRCS "src/esp_pxa_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos lvgl pxa pxa_system_core pxa_system_pxa pxa_system_standard)
