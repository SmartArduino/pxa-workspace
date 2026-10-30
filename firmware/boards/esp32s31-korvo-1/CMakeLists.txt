idf_component_register(
    SRCS
        "src/esp32s31_korvo_1_board_port.cc"
        "src/esp32s31_korvo_1_adc_calibration.cc"
        "src/esp32s31_korvo_1_hardware.cc"
        "src/esp32s31_korvo_1_pxa_surface.cc"
        "src/esp32s31_korvo_1_audio.cc"
        "src/esp32s31_korvo_1_storage.cc"
        "src/esp32s31_korvo_1_vision.cc"
        "src/korvo_lvgl_accel.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ppa
        esp_driver_sdmmc
        esp_driver_cam
        esp_mm
        fatfs
        sdmmc
        esp_event
        esp_lcd
        esp_netif
        esp_timer
        espressif__esp_codec_dev
        espressif__esp_lcd_touch
        espressif__esp_lcd_touch_gt1151
        espressif__esp_lv_decoder
        espressif__esp_lvgl_port
        espressif__button
        espressif__led_strip
        espressif__esp_cam_sensor
        espressif__esp_sccb_intf
        joltwallet__littlefs
        lvgl
        pxa
        pxadb
        pxa_board_api
        pxa_board_common
        pxa_system_reference_ui
        pxa_system_standard)

# led_strip 2.5 uses heap declarations formerly provided by aggregate drivers.
idf_component_get_property(_korvo_led_lib espressif__led_strip COMPONENT_LIB)
target_link_libraries(${_korvo_led_lib} PRIVATE idf::heap)
target_compile_options(${_korvo_led_lib} PRIVATE -include esp_heap_caps.h)

option(PXA_KORVO_TOUCH_TRACE "Log physical touch reports and edges for device verification" OFF)
option(PXA_KORVO_LVGL_PIE "Use exact Core1 PIE ARGB8888 to RGB565 block blending" OFF)
option(PXA_KORVO_LVGL_DIAGNOSTICS "Log LVGL accelerator hits and CPU runtime counters" OFF)
option(PXA_KORVO_LVGL_SELF_TEST "Compare the PIE renderer against native LVGL on startup" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_KORVO_LVGL_PIE=$<BOOL:${PXA_KORVO_LVGL_PIE}>
    PXA_KORVO_LVGL_DIAGNOSTICS=$<BOOL:${PXA_KORVO_LVGL_DIAGNOSTICS}>
    PXA_KORVO_LVGL_SELF_TEST=$<BOOL:${PXA_KORVO_LVGL_SELF_TEST}>)
set_source_files_properties("src/korvo_lvgl_accel.c" PROPERTIES COMPILE_OPTIONS "-O3")
if(PXA_KORVO_LVGL_PIE OR PXA_KORVO_LVGL_DIAGNOSTICS)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_draw_sw_blend_image_to_rgb565")
endif()
if(PXA_KORVO_LVGL_PIE)
    target_sources(${COMPONENT_LIB} PRIVATE "src/korvo_lvgl_pie.S")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore")
endif()
if(PXA_KORVO_LVGL_SELF_TEST)
    if(NOT PXA_KORVO_LVGL_PIE)
        message(FATAL_ERROR "PXA_KORVO_LVGL_SELF_TEST requires PXA_KORVO_LVGL_PIE")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "src/korvo_lvgl_accel_test.c")
endif()
if(PXA_KORVO_TOUCH_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_KORVO_TOUCH_TRACE=1)
endif()

# esp_lv_decoder includes LVGL's private PPA declarations when LV_USE_PPA is
# enabled, so its standalone decoder target also needs the PPA include path.
if(CONFIG_LV_USE_PPA AND TARGET lvgl_docoder_lib)
    target_link_libraries(lvgl_docoder_lib PRIVATE idf::esp_driver_ppa)
endif()

# PNG icons do not use the JPEG engine; reserving it at decoder init leaves
# too little internal SRAM for a running PXA application on this board.
if(TARGET lvgl_docoder_lib)
    target_compile_definitions(lvgl_docoder_lib PRIVATE ESP_LV_ENABLE_HW_JPEG=0)
endif()

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
    if(TARGET lvgl_docoder_lib)
        target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
    endif()
endif()
