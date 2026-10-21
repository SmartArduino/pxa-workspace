# Point a comparison at an isolated App checkout without changing the workspace.
function(pxa_profile_add_quake)
    if(DEFINED ENV{PXA_GAME_APPS_ROOT} AND NOT "$ENV{PXA_GAME_APPS_ROOT}" STREQUAL "")
        set(game_apps_root "$ENV{PXA_GAME_APPS_ROOT}")
    else()
        get_filename_component(game_apps_root
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../../local/pxa-apps" ABSOLUTE)
    endif()
    if(NOT EXISTS "${game_apps_root}/quake/CMakeLists.txt")
        message(FATAL_ERROR "PXA_GAME_APPS_ROOT must contain the Quake App checkout")
    endif()
    # Preserve historical comparison settings when canonical defaults change.
    foreach(feature PXA_RECROF_MAIN_LOOP PXA_RECROF_NATIVE_PCM PXA_RECROF_HOST_BLIT PXA_RECROF_BULK_READ_COPY PXA_RECROF_PACKED_GAME_CODEC PXA_QUAKE_LIT_CACHE PXA_QUAKE_LIGHTMAP_CACHE)
        if(NOT DEFINED ${feature})
            set(${feature} OFF)
        endif()
    endforeach()
    add_subdirectory("${game_apps_root}/quake" engine)
endfunction()
