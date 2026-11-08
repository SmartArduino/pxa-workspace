set(accel_sources "pixels.c" "ppa.c" "self_test.c" "lvgl.c")
set(accel_dependencies esp_timer freertos)
if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND accel_sources "pixels_s31.S")
    # IDF discovers dependencies before CONFIG_* is available. The CORDIC
    # driver is only referenced (and linked) by the optional self-test below.
    list(APPEND accel_dependencies esp_driver_ppa esp_driver_cordic esp_mm pthread)
    if(CONFIG_PXA_RENDER_ACCEL_SELF_TEST)
        list(APPEND accel_sources "cordic_test.c")
    endif()
endif()
idf_component_register(SRCS ${accel_sources} INCLUDE_DIRS "include"
                       REQUIRES lvgl PRIV_REQUIRES ${accel_dependencies})
if(CONFIG_PXA_RENDER_ACCEL)
    # LVGL refers back to this optional hook after the accelerator archive may
    # already have been scanned by the linker. Keep that entry point reachable.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,pxa_render_lvgl_fill")
    idf_component_get_property(accel_lvgl lvgl__lvgl COMPONENT_LIB)
    target_include_directories(${accel_lvgl} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_compile_definitions(${accel_lvgl} PUBLIC
        LV_USE_DRAW_SW_ASM=255
        LV_DRAW_SW_ASM_CUSTOM_INCLUDE="pxa_render_accel_lvgl.h")
endif()
