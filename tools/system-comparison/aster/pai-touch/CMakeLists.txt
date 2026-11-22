cmake_minimum_required(VERSION 3.20)
list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/../../../modules/wamr/build-scripts/esp-idf"
  "${CMAKE_CURRENT_LIST_DIR}/../../../systems/espidf/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(WAMR_BUILD_THREAD_MGR 1 CACHE BOOL "Aster runtime thread manager" FORCE)
project(aster_pai_touch)
idf_component_get_property(WAMR_LIB wamr COMPONENT_LIB)
get_target_property(WAMR_SOURCES ${WAMR_LIB} SOURCES)
list(FILTER WAMR_SOURCES EXCLUDE REGEX "/espidf_memmap.c$")
set_property(TARGET ${WAMR_LIB} PROPERTY SOURCES "${WAMR_SOURCES}")
target_sources(${WAMR_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/main/wamr_memmap.c")
idf_component_get_property(MMU_LIB esp_mm COMPONENT_LIB)
target_link_libraries(${WAMR_LIB} PRIVATE ${MMU_LIB})
# IDF 6.2 supplies nanosleep; keep WAMR's older private implementation local.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../../modules/wamr/core/shared/platform/esp-idf/espidf_platform.c"
  TARGET_DIRECTORY ${WAMR_LIB} APPEND PROPERTY COMPILE_DEFINITIONS "nanosleep=aster_wamr_nanosleep")
fatfs_create_spiflash_image(storage "${CMAKE_CURRENT_LIST_DIR}/seed" FLASH_IN_PROJECT)
