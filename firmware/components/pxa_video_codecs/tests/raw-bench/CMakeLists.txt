cmake_minimum_required(VERSION 3.16)

foreach(input IN ITEMS OPENH264_DIR TINYUSB_COMPONENTS CLIP_0 CLIP_1)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Set ${input} to the corresponding local path")
    endif()
endforeach()

set(IDF_TARGET esp32s31 CACHE STRING "Benchmark target")
set(ENV{IDF_COMPONENT_MANAGER} "0")
set(EXTRA_COMPONENT_DIRS "${OPENH264_DIR}"
    "${TINYUSB_COMPONENTS}/esp_tinyusb" "${TINYUSB_COMPONENTS}/tinyusb")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(pxa_raw_h264_bench)
