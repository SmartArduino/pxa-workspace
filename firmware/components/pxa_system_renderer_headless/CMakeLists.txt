if(NOT DEFINED PXSYS_ROOT)
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../deps/pxa-system" ABSOLUTE)
endif()
include("${PXSYS_ROOT}/ui/renderers/headless/CMakeLists.txt")
