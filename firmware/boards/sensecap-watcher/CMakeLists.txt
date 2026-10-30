idf_component_register(
    SRCS
        "src/sensecap_watcher_board_port.cc"
        "src/watcher_hardware.cc"
        "src/watcher_io_expander.cc"
        "src/watcher_battery.cc"
        "src/watcher_audio.cc"
        "src/watcher_storage.cc"
        "src/watcher_rtc.cc"
        "src/watcher_vision.cc"
        "src/watcher_vision_reply.cc"
        "vendor/sscma-spi/sscma_client_io_spi.c"
        "src/watcher_pxa_surface.cc"
        "src/watcher_lcd_io.cc"
        "src/watcher_rgb565_swap.S"
        "src/watcher_spd2010_touch.c"
    INCLUDE_DIRS "include" "vendor/sscma-spi"
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_spi
        esp_driver_sdspi
        fatfs
        mbedtls
        espressif__cjson
        sdmmc
        esp_event
        esp_lcd
        esp_timer
        espressif__button
        espressif__esp_codec_dev
        espressif__esp_lcd_spd2010
        espressif__esp_lcd_touch
        espressif__esp_lv_decoder
        espressif__esp_lvgl_port
        espressif__knob
        espressif__led_strip
        lvgl
        pxa
        pxa_board_api
        pxa_board_common
        pxa_system_clock
        pxa_lvgl_memory
        pxadb
        pxa_system_reference_ui
        pxa_system_standard)

# knob 0.1.4 keeps its stable API but still depends on IDF's former aggregate
# driver component. IDF 6 split GPIO out; attach the real build dependency
# here without editing downloaded managed-component sources.
idf_component_get_property(_watcher_knob_lib espressif__knob COMPONENT_LIB)
target_link_libraries(${_watcher_knob_lib} PRIVATE idf::esp_driver_gpio)
# The pinned codec driver uses the same aggregate-driver convention.
idf_component_get_property(_watcher_codec_lib espressif__esp_codec_dev COMPONENT_LIB)
target_link_libraries(${_watcher_codec_lib} PRIVATE
    idf::esp_driver_gpio idf::esp_driver_i2c idf::esp_driver_i2s idf::esp_driver_spi)
# The codec's SPI source tests version macros without including their header;
# newer IDF driver headers no longer provide that accidental transitive include.
target_compile_options(${_watcher_codec_lib} PRIVATE -include esp_idf_version.h)
# led_strip 2.5 also relies on heap declarations formerly included by SPI.
idf_component_get_property(_watcher_led_lib espressif__led_strip COMPONENT_LIB)
target_link_libraries(${_watcher_led_lib} PRIVATE idf::heap)
target_compile_options(${_watcher_led_lib} PRIVATE -include esp_heap_caps.h)

# Optimize the scanout kernels for speed; all cold board setup keeps the IDF
# build's selected optimization level.
set_source_files_properties("src/watcher_pxa_surface.cc" PROPERTIES COMPILE_OPTIONS "-O3")

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
    # The decoder includes LVGL's private headers, which in turn include
    # ft2build.h; its own target needs the FreeType include directories too.
    if(TARGET lvgl_docoder_lib)
        target_link_libraries(lvgl_docoder_lib PRIVATE idf::espressif__freetype)
    endif()
endif()

# ---------------------------------------------------------------------------
# LVGL's allocator lives in pxa_lvgl_memory. LVGL cannot name that component
# itself (it is a managed component), and the linker resolves archive members
# left to right, so link it *behind* LVGL: otherwise lv_malloc_core and friends
# are still undefined when LVGL is pulled in.
# ---------------------------------------------------------------------------
idf_component_get_property(_pxa_lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(_pxa_lvgl_lib)
    target_link_libraries(${_pxa_lvgl_lib} PUBLIC idf::pxa_lvgl_memory)
endif()
