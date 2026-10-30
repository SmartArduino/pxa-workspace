# SPDX-License-Identifier: Apache-2.0
if(NOT TARGET pxa_peripherals_sdk)
    add_library(pxa_peripherals_sdk INTERFACE)
    add_library(pxa::peripherals ALIAS pxa_peripherals_sdk)
    target_include_directories(pxa_peripherals_sdk INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/../include")
endif()
