if(NOT DEFINED PXSYS_ROOT)
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../deps/pxa-system" ABSOLUTE)
endif()
if(NOT EXISTS "${PXSYS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "PXSYS_ROOT must point to the imported pxa-system")
endif()
include("${PXSYS_ROOT}/ui/renderers/lvgl/CMakeLists.txt")
