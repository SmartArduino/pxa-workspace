# The JL701 coprocessor stack used to be two shared components
# (`rpc_701` + `rpc_adapter`) pulled in as submodules. pai-touch is its only
# consumer, so the vendored sources now compile into this board component and
# the RPC headers stay private to it; `jl701_audio_link` is the only audio
# surface the board exposes.
include("${CMAKE_CURRENT_LIST_DIR}/vendor/rpc701/pai_touch_rpc701.cmake")

idf_component_register(
    SRCS
        "src/pai_touch_board_port.cc"
        "src/pai_touch_hardware.cc"
        "src/pai_touch_audio.cc"
        "src/jl701_audio_link.cc"
        "src/adc_battery_monitor.cc"
        "src/button.cc"
        "src/te_sync_panel.cc"
        ${PXA_RPC701_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${PXA_RPC701_INCLUDE_DIR}
    PRIV_REQUIRES
        78__esp-wifi-connect
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_driver_uart
        esp_event
        esp_lcd
        esp_netif
        esp_ringbuf
        esp_timer
        espressif__adc_battery_estimation
        espressif__button
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        esp_lcd_touch_cst826
        joltwallet__littlefs
        lvgl
        nvs_flash
        pxa
        pxadb
        pxa_board_api
        pxa_board_common
        pxa_system_reference_ui
        pxa_system_standard)

# This board owns the direct scanout presenter. It is not part of the generic
# port API and remains selected only for the pai-touch build profile.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG=1)

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
endif()
