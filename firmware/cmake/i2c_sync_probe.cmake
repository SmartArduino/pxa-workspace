# Keep the reviewed IDF receive/probe lifetime fix local to this build.
# The external ESP-IDF checkout is never modified.
idf_component_get_property(pxa_i2c_component esp_driver_i2c COMPONENT_LIB)
idf_component_get_property(pxa_i2c_directory esp_driver_i2c COMPONENT_DIR)
idf_build_get_property(pxa_i2c_python PYTHON)
set(pxa_i2c_prepare "${CMAKE_CURRENT_LIST_DIR}/../../tools/prepare_idf_i2c.py")
get_filename_component(pxa_i2c_prepare "${pxa_i2c_prepare}" ABSOLUTE)
set(pxa_i2c_output "${CMAKE_BINARY_DIR}/idf-i2c/i2c_master.c")
execute_process(
    COMMAND "${pxa_i2c_python}" "${pxa_i2c_prepare}"
        "${pxa_i2c_directory}/i2c_master.c" "${pxa_i2c_output}"
    RESULT_VARIABLE pxa_i2c_result)
if(NOT pxa_i2c_result EQUAL 0)
    message(FATAL_ERROR "ESP-IDF I2C source review failed")
endif()
get_target_property(pxa_i2c_sources ${pxa_i2c_component} SOURCES)
set(pxa_i2c_replaced OFF)
foreach(pxa_i2c_source IN LISTS pxa_i2c_sources)
    get_filename_component(pxa_i2c_name "${pxa_i2c_source}" NAME)
    if(pxa_i2c_name STREQUAL "i2c_master.c")
        list(REMOVE_ITEM pxa_i2c_sources "${pxa_i2c_source}")
        set(pxa_i2c_replaced ON)
    endif()
endforeach()
if(NOT pxa_i2c_replaced)
    message(FATAL_ERROR "ESP-IDF I2C source target changed")
endif()
list(APPEND pxa_i2c_sources "${pxa_i2c_output}")
set_property(TARGET ${pxa_i2c_component} PROPERTY SOURCES "${pxa_i2c_sources}")
target_include_directories(${pxa_i2c_component} PRIVATE "${pxa_i2c_directory}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${pxa_i2c_prepare}" "${pxa_i2c_directory}/i2c_master.c")
