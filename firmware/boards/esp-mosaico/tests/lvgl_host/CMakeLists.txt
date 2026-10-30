cmake_minimum_required(VERSION 3.16)
project(mosaico_lvgl_regressions LANGUAGES C CXX)

find_package(SDL2 REQUIRED)
find_package(Freetype REQUIRED)
find_package(PNG REQUIRED)
get_filename_component(workspace "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${workspace}/deps/pxa-system/simulator/desktop/lv_conf.h"
    CACHE PATH "LVGL host configuration")
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# The ordinary simulator disables recognizers. Exercise the device's enabled
# path too; float is a required dependency of the LVGL gesture implementation.
add_compile_definitions(LV_USE_GESTURE_RECOGNITION=1 LV_USE_FLOAT=1)
add_subdirectory("${workspace}/firmware/managed_components/lvgl__lvgl" lvgl EXCLUDE_FROM_ALL)
target_include_directories(lvgl PUBLIC "${workspace}/deps/pxa-system/simulator/desktop")
target_link_libraries(lvgl PUBLIC SDL2::SDL2 Freetype::Freetype PNG::PNG)

add_executable(mosaico_test "${CMAKE_CURRENT_LIST_DIR}/../lvgl_partial_test.cc")
target_include_directories(mosaico_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_features(mosaico_test PRIVATE cxx_std_17)
target_compile_options(mosaico_test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(mosaico_test PRIVATE lvgl m)

add_executable(mosaico_direct_refresh_test "${CMAKE_CURRENT_LIST_DIR}/../lvgl_direct_refresh_test.cc")
target_include_directories(mosaico_direct_refresh_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_features(mosaico_direct_refresh_test PRIVATE cxx_std_17)
target_compile_options(mosaico_direct_refresh_test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_link_libraries(mosaico_direct_refresh_test PRIVATE lvgl m)
