# The JL701 coprocessor stack used to be two shared components
# (`rpc_701` + `rpc_adapter`) pulled in as submodules. pai-touch is its only
# consumer, so the vendored sources now compile into this board component and
# the RPC headers stay private to it; `jl701_audio_link` is the only audio
# surface the board exposes.
include("${CMAKE_CURRENT_LIST_DIR}/vendor/rpc701/pai_touch_rpc701.cmake")
idf_build_get_property(PXA_VOICE_TRIGGER_ENABLED PXA_ENABLE_VOICE_TRIGGER)
set(PXA_PAI_VOICE_TRIGGER_SOURCES "")
if(PXA_VOICE_TRIGGER_ENABLED)
    list(APPEND PXA_PAI_VOICE_TRIGGER_SOURCES "src/pai_touch_voice_trigger.cc")
endif()

idf_component_register(
    SRCS
        "src/pai_touch_board_port.cc"
        "src/pai_touch_hardware.cc"
        "src/pai_touch_camera.cc"
        "src/pai_touch_camera_dma.c"
        "src/pai_touch_display_dma.cc"
        "src/pai_touch_audio.cc"
        "src/jl701_audio_link.cc"
        "src/adc_battery_monitor.cc"
        "src/button.cc"
        "src/te_sync_panel.cc"
        ${PXA_PAI_VOICE_TRIGGER_SOURCES}
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
        esp_hw_support
        esp_lcd
        esp_netif
        esp_ringbuf
        esp_timer
        espressif__adc_battery_estimation
        espressif__button
        espressif__esp_lcd_touch
        espressif__esp_lvgl_port
        espressif__esp32-camera
        espressif__esp_new_jpeg
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
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--undefined=__wrap_ll_cam_dma_sizes"
    "LINKER:--wrap=ll_cam_dma_sizes"
    "LINKER:--wrap=spi_bus_add_device"
    "LINKER:--wrap=spi_bus_get_max_transaction_len"
    "LINKER:--wrap=spi_device_get_trans_result"
    "LINKER:--wrap=spi_device_queue_trans")
set(PXA_PAI_DISPLAY_DMA_TEST_FAULTS 0 CACHE STRING "Test-only completed LCD DMA faults to inject at boot")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_PAI_DISPLAY_DMA_TEST_FAULTS=${PXA_PAI_DISPLAY_DMA_TEST_FAULTS})
set(PXA_PAI_CAMERA_DIAGNOSTIC 0 CACHE STRING "Camera diagnostic raw capture and encoder color bars")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PXA_PAI_CAMERA_DIAGNOSTIC=${PXA_PAI_CAMERA_DIAGNOSTIC})
if(PXA_AUDIO_LATENCY_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_AUDIO_LATENCY_TRACE=1)
endif()

if(CONFIG_LV_USE_FREETYPE)
    idf_component_get_property(LVGL_COMPONENT_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${LVGL_COMPONENT_LIB} PRIVATE idf::espressif__freetype)
endif()

# The RGB/JPEG worker's measured stack fits 4 KiB. Keep internal SRAM
# available for the camera DMA ring instead of reserving it in an idle stack.
idf_component_get_property(PAI_BOARD_COMMON_LIB pxa_board_common COMPONENT_LIB)
target_compile_definitions(${PAI_BOARD_COMMON_LIB} PRIVATE PXA_BOARD_VISION_STACK_BYTES=4096)

# esp32-camera is pinned to 2.1.8. The DMA adapter uses its private geometry
# hook, with the original header and ABI compiled into this board only.
idf_component_get_property(PAI_CAMERA_COMPONENT_DIR espressif__esp32-camera COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${PAI_CAMERA_COMPONENT_DIR}/target/private_include")
