if(NOT DEFINED PXSYS_ROOT)
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../deps/pxa-system" ABSOLUTE)
endif()
if(NOT EXISTS "${PXSYS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "PXSYS_ROOT must point to the imported pxa-system")
endif()
include("${PXSYS_ROOT}/cmake/PxaSystemSources.cmake")

idf_component_register(
    SRCS ${PXSYS_REFERENCE_LAYOUT_SOURCES} ${PXSYS_REFERENCE_LVGL_SOURCES}
    INCLUDE_DIRS
        "${PXSYS_ROOT}/ui/reference/include"
        "${PXSYS_ROOT}/ui/reference/lvgl/include"
    REQUIRES lvgl pxa_system_core pxa_system_native pxa_system_standard)

target_compile_features(${COMPONENT_LIB} PUBLIC c_std_99)

function(pxsys_reference_ui_define_bool config_symbol define_symbol)
    if(${config_symbol})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${define_symbol}=1)
    else()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${define_symbol}=0)
    endif()
endfunction()

pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_ANIMATIONS PXSYS_REFERENCE_UI_ENABLE_ANIMATIONS)
pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_WIFI PXSYS_REFERENCE_UI_WIFI)
pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_CELLULAR PXSYS_REFERENCE_UI_CELLULAR)
pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_BATTERY_PERCENT PXSYS_REFERENCE_UI_BATTERY_PERCENT)
pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_GESTURE_HANDLE PXSYS_REFERENCE_UI_GESTURE_HANDLE)
pxsys_reference_ui_define_bool(CONFIG_PXSYS_REFERENCE_UI_WALLPAPER PXSYS_REFERENCE_UI_ENABLE_WALLPAPER)
if(DEFINED CONFIG_PXSYS_REFERENCE_UI_LAUNCHER_COLUMNS AND DEFINED CONFIG_PXSYS_REFERENCE_UI_LAUNCHER_ROWS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        PXSYS_REFERENCE_UI_LAUNCHER_COLUMNS=${CONFIG_PXSYS_REFERENCE_UI_LAUNCHER_COLUMNS}
        PXSYS_REFERENCE_UI_LAUNCHER_ROWS=${CONFIG_PXSYS_REFERENCE_UI_LAUNCHER_ROWS})
endif()
if(CONFIG_PXSYS_REFERENCE_UI_TASK_SWITCHER_CARDS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        PXSYS_REFERENCE_UI_TASK_SWITCHER=PXSYS_TASK_SWITCHER_CARDS
        PXSYS_REFERENCE_UI_TASK_PREVIEW_COUNT=${CONFIG_PXSYS_REFERENCE_UI_TASK_PREVIEW_COUNT})
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXSYS_REFERENCE_UI_TASK_SWITCHER=PXSYS_TASK_SWITCHER_LIST)
endif()
