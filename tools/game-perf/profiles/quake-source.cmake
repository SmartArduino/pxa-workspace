# Point a comparison at an isolated App checkout without changing the workspace.
function(pxa_profile_add_quake)
    if(DEFINED ENV{PXA_GAME_APPS_ROOT} AND NOT "$ENV{PXA_GAME_APPS_ROOT}" STREQUAL "")
        set(game_apps_root "$ENV{PXA_GAME_APPS_ROOT}")
    else()
        get_filename_component(game_apps_root
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../local/pxa-apps" ABSOLUTE)
    endif()
    if(NOT EXISTS "${game_apps_root}/quake-sd/CMakeLists.txt")
        message(FATAL_ERROR "PXA_GAME_APPS_ROOT must contain the Quake SD App checkout")
    endif()
    add_subdirectory("${game_apps_root}/quake-sd" engine)
endfunction()
