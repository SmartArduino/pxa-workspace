if(NOT DEFINED PXSYS_ROOT)
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../deps/pxa-system" ABSOLUTE)
endif()

idf_build_get_property(pxa_board_component PXA_BOARD_COMPONENT)

idf_component_register(SRCS "app_main.cc"
    INCLUDE_DIRS "."
    REQUIRES ${pxa_board_component} pxa_integration pxa_board_api pxadb
    PRIV_REQUIRES nvs_flash)
