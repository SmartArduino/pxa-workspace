# SPDX-License-Identifier: Apache-2.0
if(NOT TARGET pxa_voice_trigger_sdk)
    add_library(pxa_voice_trigger_sdk INTERFACE)
    add_library(pxa::voice_trigger ALIAS pxa_voice_trigger_sdk)
    target_include_directories(pxa_voice_trigger_sdk INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/include"
        "${CMAKE_CURRENT_LIST_DIR}/../include")
endif()
