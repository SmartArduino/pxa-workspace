cmake_minimum_required(VERSION 3.16)

set(PXSYS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../deps/pxa-system" CACHE PATH
    "Path to the imported pxa-system checkout")
set(PXA_BOARD "pai-touch" CACHE STRING "Selected board profile")

if(NOT EXISTS "${PXSYS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "PXSYS_ROOT must contain an initialized pxa-system checkout")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${PXA_BOARD}/CMakeLists.txt")
    message(FATAL_ERROR "Unknown PXA_BOARD: ${PXA_BOARD}")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${PXA_BOARD}/board.cmake")
    message(FATAL_ERROR "Board ${PXA_BOARD} must provide board.cmake metadata")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/boards/${PXA_BOARD}/board.cmake")
if(NOT DEFINED PXA_BOARD_TARGET)
    message(FATAL_ERROR "Board ${PXA_BOARD} must set PXA_BOARD_TARGET")
endif()
if(NOT DEFINED PXA_BOARD_COMPONENT)
    message(FATAL_ERROR "Board ${PXA_BOARD} must set PXA_BOARD_COMPONENT")
endif()

# The target must be known before ESP-IDF starts component discovery. Board
# builds have independent build directories, so the board is authoritative.
set(IDF_TARGET "${PXA_BOARD_TARGET}" CACHE STRING "ESP-IDF target" FORCE)

add_compile_options(-Wno-missing-field-initializers)

# Board components are selected before ESP-IDF discovers components.  This is
# intentionally a CMake choice rather than a global CONFIG_BOARD_TYPE choice.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/boards/${PXA_BOARD}"
    "${PXSYS_ROOT}/wamr")

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/boards/${PXA_BOARD}/sdkconfig.defaults")

# Keep the generated Kconfig state per board. The shared firmware/sdkconfig
# would otherwise leak flash size, partition table and feature choices from a
# previously built board into the next one.
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${PXA_BOARD}")
endif()

# Firmware and Host capability profiles share the SDK release identity. Git's
# nearest tag may be a separately versioned tool release (for example
# host-tools-v0.1.0), so it cannot determine the firmware application version.
file(STRINGS "${PXSYS_ROOT}/VERSION" PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PXSYS_ROOT}/VERSION")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Each board can resolve a different managed-component graph. Keep its exact
# resolution in a separate lock file instead of rewriting another board's.
idf_build_set_property(DEPENDENCIES_LOCK "dependencies.lock.${PXA_BOARD}")
# Component CMakeLists files are expanded in a separate CMake process. Publish
# the selected board component through an ESP-IDF build property instead of
# relying on the top-level PXA_BOARD cache variable in that process.
idf_build_set_property(PXA_BOARD_COMPONENT "${PXA_BOARD_COMPONENT}")
idf_build_set_property(PXA_BOARD_ID "${PXA_BOARD}")
idf_build_set_property(PXA_ENABLE_VOICE_TRIGGER "${PXA_ENABLE_VOICE_TRIGGER}")
idf_build_set_property(PXA_ENABLE_PERIPHERALS "${PXA_BOARD_ENABLE_PERIPHERALS}")
idf_build_set_property(MINIMAL_BUILD ON)
# The generic ESP PXA host has no app_pages dependency. Legacy products can
# provide their own compatibility component without changing this project.
idf_build_set_property(PXA_ENABLE_APP_PAGES_COMPAT OFF)

project(pxa_esp_platform)

include("${PXSYS_ROOT}/cmake/PxaLicensePolicy.cmake")
if(CONFIG_LV_USE_FS_FROGFS OR CONFIG_LV_USE_THORVG)
    message(FATAL_ERROR "PXA system license policy prohibits bundled FrogFS/ThorVG implementations")
endif()
if(TARGET __idf_lvgl__lvgl)
    pxa_restrict_lvgl(__idf_lvgl__lvgl)
endif()


if(CONFIG_PXA_ENABLED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(pxa_system_license_gate
        COMMAND "${Python3_EXECUTABLE}"
            "${PXSYS_ROOT}/tools/licensing/check_system_licenses.py"
            --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
            --source-root "${PXSYS_ROOT}/libpxa"
            --source-root "${PXSYS_ROOT}/system"
            --source-root "${PXSYS_ROOT}/ui"
            --source-root "${PXSYS_ROOT}/platforms"
        COMMENT "Checking compiled system license policy"
        VERBATIM)
    add_dependencies(__idf_pxa pxa_system_license_gate)
    idf_build_get_property(pxa_python PYTHON)
    idf_build_get_property(pxa_elf EXECUTABLE)
    get_filename_component(pxa_compiler_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(pxa_toolchain_root "${pxa_compiler_bin}/.." ABSOLUTE)
    add_custom_command(TARGET ${pxa_elf} POST_BUILD
        COMMAND "${pxa_python}" "${PXSYS_ROOT}/tools/licensing/check_system_licenses.py"
            --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
            --ninja-dependencies --toolchain-root "${pxa_toolchain_root}"
        COMMENT "Checking compiled headers and runtime license exceptions"
        VERBATIM)

    add_custom_command(TARGET ${pxa_elf} POST_BUILD
        COMMAND "${pxa_python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/audit-system-binaries.py"
            --map "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
            --idf-root "$ENV{IDF_PATH}"
            --firmware-root "${CMAKE_CURRENT_LIST_DIR}"
            --compiler "${CMAKE_C_COMPILER}"
        COMMENT "Checking linked prebuilt system licenses"
        VERBATIM)
    set_property(TARGET ${pxa_elf} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/audit-system-binaries.py"
        "${CMAKE_CURRENT_LIST_DIR}/../tools/system-binary-licenses.json")

    add_custom_command(TARGET ${pxa_elf} POST_BUILD
        COMMAND "${pxa_python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/audit-codec-memory.py"
            --elf "$<TARGET_FILE:${pxa_elf}>"
            --map "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
            --codec-dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_audio_codec/lib/${IDF_TARGET}"
            --readelf "${CMAKE_READELF}"
            --report "${CMAKE_BINARY_DIR}/codec-memory-audit.json"
        COMMENT "Checking codec allocations use the shared resource budget"
        VERBATIM)
    set_property(TARGET ${pxa_elf} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/audit-codec-memory.py")
endif()
