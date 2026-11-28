set(board "${CMAKE_CURRENT_LIST_DIR}/../../../../../boards/esp-mosaico")
idf_component_register(
    SRCS "raw_bench.cpp" "${board}/src/esp_mosaico_usb_restart.cc"
    PRIV_INCLUDE_DIRS "${board}/include" "../../../src"
    PRIV_REQUIRES esp_openh264 esp_tinyusb esp_timer esp_hw_support esp_hal_usb esp_driver_ppa)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_cpu_reset" "-Wl,--undefined=__wrap_esp_cpu_reset")
foreach(index RANGE 0 1)
    set(clip "${CMAKE_CURRENT_BINARY_DIR}/clip_${index}.bin")
    configure_file("${CLIP_${index}}" "${clip}" COPYONLY)
    target_add_binary_data(${COMPONENT_LIB} "${clip}" BINARY)
endforeach()
