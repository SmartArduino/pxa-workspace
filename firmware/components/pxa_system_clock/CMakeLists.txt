idf_component_register(SRCS "pxa_system_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash pxa_system_core)
