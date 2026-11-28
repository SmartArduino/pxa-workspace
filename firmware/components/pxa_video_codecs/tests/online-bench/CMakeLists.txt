cmake_minimum_required(VERSION 3.16)

set(BILIBILI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../local/pxa-apps/bilibili"
    CACHE PATH "Billy source directory")
if(NOT EXISTS "${OPENH264_DIR}/CMakeLists.txt" OR NOT EXISTS "${BILIBILI_DIR}/dash.hpp")
    message(FATAL_ERROR "Set OPENH264_DIR and BILIBILI_DIR to the local library and Billy sources")
endif()

set(IDF_TARGET esp32s31 CACHE STRING "Benchmark target")
set(ENV{IDF_COMPONENT_MANAGER} "0")
set(EXTRA_COMPONENT_DIRS "${OPENH264_DIR}")
foreach(component IN ITEMS esp_tinyusb tinyusb esp_audio_codec esp_codec_dev esp_lcd_co5300)
    if(NOT EXISTS "${BOARD_COMPONENTS}/${component}/CMakeLists.txt")
        message(FATAL_ERROR "Missing ${component} under BOARD_COMPONENTS")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${BOARD_COMPONENTS}/${component}")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${BOARD_COMPONENTS}/cmake_utilities")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(mosaico_online_h264)
