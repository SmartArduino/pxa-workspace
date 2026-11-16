# Generate the storage-stat adapter for both workspace and Host SDK products.
function(pxa_configure_littlefs_usage component_dir)
    idf_component_get_property(library joltwallet__littlefs COMPONENT_LIB)
    idf_component_get_property(driver joltwallet__littlefs COMPONENT_DIR)
    idf_build_get_property(python PYTHON)
    set(tool "${component_dir}/tools/prepare_littlefs_usage.py")
    set(source "${driver}/src/esp_littlefs.c")
    set(output "${CMAKE_BINARY_DIR}/pxa-littlefs-usage/esp_littlefs.c")
    execute_process(COMMAND "${python}" "${tool}" "${source}" "${output}"
        COMMAND_ERROR_IS_FATAL ANY OUTPUT_QUIET)
    get_target_property(sources ${library} SOURCES)
    set(matches 0)
    foreach(item IN LISTS sources)
        get_filename_component(name "${item}" NAME)
        if(name STREQUAL "esp_littlefs.c")
            if(IS_ABSOLUTE "${item}")
                set(original "${item}")
            else()
                set(original "${driver}/${item}")
            endif()
            # The managed driver applies LFS_CONFIG per source, including its
            # allocator and log configuration. Preserve that compilation scope.
            foreach(property COMPILE_FLAGS COMPILE_OPTIONS COMPILE_DEFINITIONS
                             INCLUDE_DIRECTORIES LANGUAGE)
                get_source_file_property(value "${original}"
                    TARGET_DIRECTORY ${library} ${property})
                if(NOT value STREQUAL "NOTFOUND")
                    set_source_files_properties("${output}"
                        TARGET_DIRECTORY ${library} PROPERTIES ${property} "${value}")
                endif()
            endforeach()
            list(REMOVE_ITEM sources "${item}")
            math(EXPR matches "${matches} + 1")
        endif()
    endforeach()
    if(NOT matches EQUAL 1)
        message(FATAL_ERROR "Expected exactly one reviewed LittleFS driver source")
    endif()
    set_property(TARGET ${library} PROPERTY SOURCES ${sources} "${output}")
    target_include_directories(${library} PRIVATE "${driver}/src" "${driver}/src/littlefs"
        "${component_dir}/src/services")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tool}"
        "${source}" "${component_dir}/src/services/pxa_littlefs_usage.h")
endfunction()
