# Component requirements are collected before sdkconfig is finalized. Keep
# this registration stable so enabling PXA cannot add sources without their
# include paths and linked components.
set(PXA_SRCS
    "src/runtime/pxa_host.c"
    "src/runtime/pxa_host_activation_arena.c"
    "src/runtime/pxa_host_clock_slots.c"
    "src/runtime/pxa_host_pointer_mailbox.c"
    "src/runtime/pxa_esp_host.c"
    "src/services/pxa_esp_audio.c"
    "src/services/pxa_esp_net.c"
    "src/services/pxa_esp_services.c"
    "src/services/pxa_esp_surface.c"
    "src/package/pxa_development_trust.c"
    "src/package/pxa_package_disabled_policy.c"
    "src/package/pxa_esp_package_icon.c"
    "src/package/pxa_esp_package_policy.c"
    "src/package/pxa_esp_package_store.c"
    "src/package/pxa_esp_store_download.c"
    "src/package/pxa_esp_permission_store.c"
    "src/ui/pxa_esp_ui_assets.c"
    "src/ui/pxa_esp_store_result.c")

# Product builds retain the legacy app_pages shell unless they explicitly opt
# into the standalone PXA System shell before ESP-IDF component discovery.
idf_build_get_property(PXA_ENABLE_APP_PAGES_COMPAT
                       PXA_ENABLE_APP_PAGES_COMPAT)
if("${PXA_ENABLE_APP_PAGES_COMPAT}" STREQUAL "")
    set(PXA_ENABLE_APP_PAGES_COMPAT ON)
endif()
if(PXA_ENABLE_APP_PAGES_COMPAT)
    list(APPEND PXA_SRCS
        "src/package/pxa_esp_package_catalog.c"
        "src/ui/pxa_esp_app_pages.c")
else()
    list(APPEND PXA_SRCS "src/ui/pxa_esp_standalone.c")
endif()

if(NOT DEFINED PXSYS_ROOT)
    # Component requirements are collected in a separate CMake process where
    # root-project cache variables are intentionally unavailable.
    get_filename_component(PXSYS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../deps/pxa-system" ABSOLUTE)
endif()
if(NOT EXISTS "${PXSYS_ROOT}/libpxa")
    message(FATAL_ERROR "PXSYS_ROOT must point to the imported pxa-system")
endif()
set(PXA_C_CORE_ROOT "${PXSYS_ROOT}/libpxa")
include("${PXA_C_CORE_ROOT}/cmake/pxaSources.cmake")
pxa_collect_sources(PXA_LIB_SOURCES "${PXA_C_CORE_ROOT}")
list(APPEND PXA_SRCS
    ${PXA_LIB_SOURCES}
    "${PXA_C_CORE_ROOT}/adapters/esp/pxa_esp_mbedtls.c"
    "${PXA_C_CORE_ROOT}/adapters/esp/pxa_esp_posix_shim.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_fs.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_scheduler_store.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_storage.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_installer.c"
    "${PXA_C_CORE_ROOT}/adapters/wamr/pxa_wamr_engine.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_ui.c")
set(PXA_PRIV_REQUIRES
    mbedtls
    esp_http_client
    esp_hw_support
    joltwallet__littlefs
    nvs_flash
    esp_psram
    log
    pthread
    freertos)
list(APPEND PXA_PRIV_REQUIRES lz4)
set(PXA_REQUIRES lvgl esp_timer esp_mm)
if(PXA_ENABLE_APP_PAGES_COMPAT)
    list(APPEND PXA_REQUIRES app_pages)
endif()
idf_build_get_property(PXA_TARGET IDF_TARGET)
if(PXA_TARGET STREQUAL "esp32s3" OR PXA_TARGET STREQUAL "esp32s31")
    list(APPEND PXA_REQUIRES wamr)
endif()

idf_component_register(
    SRCS ${PXA_SRCS}
    INCLUDE_DIRS "include" "${PXA_C_CORE_ROOT}/include"
    PRIV_INCLUDE_DIRS "src/runtime"
                      "src/services"
                      "src/package"
                      "src/ui"
                      "${PXA_C_CORE_ROOT}/src"
                      "${PXA_C_CORE_ROOT}/adapters/include"
    REQUIRES ${PXA_REQUIRES}
    PRIV_REQUIRES ${PXA_PRIV_REQUIRES}
)
if(CONFIG_WAMR_ENABLE_LIBC_WASI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_WAMR_LIBC_WASI=1)
endif()
include("${PXSYS_ROOT}/cmake/PxaWamrMetadata.cmake")
pxsys_apply_wamr_metadata(${COMPONENT_LIB})

if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32S31)
    include("${PXSYS_ROOT}/platforms/esp-idf/cmake/pxsys_wamr.cmake")
    pxsys_configure_wamr_esp_idf(__idf_wamr)
endif()
