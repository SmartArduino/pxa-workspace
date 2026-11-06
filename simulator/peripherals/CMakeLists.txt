# SPDX-License-Identifier: Apache-2.0
get_filename_component(workspace "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(pxsys_desktop_host_extensions STATIC
    desktop_camera.c "${workspace}/extensions/peripherals/src/peripherals.c")
target_compile_features(pxsys_desktop_host_extensions PRIVATE c_std_11)
target_compile_definitions(pxsys_desktop_host_extensions PRIVATE PXSYS_DESKTOP_HOST_EXTENSIONS=1)
target_include_directories(pxsys_desktop_host_extensions PRIVATE
    "${workspace}/extensions/peripherals/include" "${PXSYS_DESKTOP_HOST_EXTENSION_INCLUDE_DIR}")
target_link_libraries(pxsys_desktop_host_extensions PRIVATE pxa::pxa JPEG::JPEG Threads::Threads)
target_compile_options(pxsys_desktop_host_extensions PRIVATE -Wall -Wextra -Werror)
if(BUILD_TESTING)
    add_executable(pxsys_desktop_camera_test tests/camera_test.c
        "${workspace}/extensions/peripherals/src/peripherals.c")
    target_include_directories(pxsys_desktop_camera_test PRIVATE
        "${workspace}/extensions/peripherals/include" "${PXSYS_DESKTOP_HOST_EXTENSION_INCLUDE_DIR}")
    target_link_libraries(pxsys_desktop_camera_test PRIVATE pxa::pxa JPEG::JPEG Threads::Threads)
    target_compile_options(pxsys_desktop_camera_test PRIVATE -UNDEBUG -Wall -Wextra -Werror
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pxsys_desktop_camera_test PRIVATE -fsanitize=address,undefined)
    add_test(NAME pxsys_desktop_camera_test COMMAND pxsys_desktop_camera_test)
endif()
