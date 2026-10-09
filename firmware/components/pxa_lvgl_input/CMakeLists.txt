idf_component_register(SRCS "lvgl_serialized_input.c" PRIV_REQUIRES lvgl)
if(CONFIG_PXA_ENABLED)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_indev_read")
endif()
