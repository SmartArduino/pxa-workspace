if(NOT DEFINED PXSYS_ROOT)
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../deps/pxa-system" ABSOLUTE)
endif()
idf_component_register(
    SRCS "${PXSYS_ROOT}/ui/backends/lvgl/page_manager/src/page_manager.c"
    INCLUDE_DIRS "${PXSYS_ROOT}/ui/backends/lvgl/page_manager/include"
    REQUIRES lvgl)
