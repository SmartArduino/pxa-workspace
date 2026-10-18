# Component requirements are collected before sdkconfig is finalized. Keep
# this registration stable so enabling PXA cannot add sources without their
# include paths and linked components.
set(PXA_SRCS
    "src/runtime/pxa_host.c"
    "src/runtime/pxa_host_activation_arena.c"
    "src/runtime/pxa_host_clock_slots.c"
    "src/runtime/pxa_host_pointer_mailbox.c"
    "src/runtime/pxa_esp_host.c"
    "src/runtime/pxa_esp_ime.c"
    "src/services/pxa_esp_audio.c"
    "src/services/pxa_esp_assets.c"
    "src/services/pxa_esp_resource_memory.c"
    "src/services/pxa_esp_system_font.c"
    "src/services/pxa_esp_net.c"
    "src/services/pxa_esp_services.c"
    "src/services/pxa_esp_fs_roots.c"
    "src/services/pxa_esp_surface.c"
    "src/package/pxa_development_trust.c"
    "src/package/pxa_package_disabled_policy.c"
    "src/package/pxa_esp_package_icon.c"
    "src/package/pxa_esp_package_policy.c"
    "src/package/pxa_esp_package_store.c"
    "src/package/pxa_esp_store_download.c"
    "src/package/pxa_esp_permission_store.c"
    "src/ui/pxa_esp_ui_assets.c"
    "src/ui/pxa_esp_system_overlay.c"
    "src/ui/pxa_esp_directory_picker.c"
    "src/ui/pxa_esp_store_result.c")

# Native products can supply the existing audio sinks without linking the
# standalone mixer or its global codec allocation hooks.
idf_build_get_property(PXA_BUILTIN_AUDIO_OUTPUT PXA_BUILTIN_AUDIO_OUTPUT)
if("${PXA_BUILTIN_AUDIO_OUTPUT}" STREQUAL "")
    set(PXA_BUILTIN_AUDIO_OUTPUT ON)
endif()
if(PXA_BUILTIN_AUDIO_OUTPUT)
    list(APPEND PXA_SRCS
        "src/services/pxa_audio_output.cc"
        "src/services/pxa_codec_memory.cc")
endif()

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
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_scoped_fs.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_clock.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_scheduler_store.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_storage.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_installer.c"
    "${PXA_C_CORE_ROOT}/adapters/posix/pxa_posix_ime.c"
    "${PXA_C_CORE_ROOT}/adapters/wamr/pxa_wamr_engine.c"
    "${PXA_C_CORE_ROOT}/adapters/wamr/pxa_wamr_file_reader.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_ui.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_ui_symbols.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_image.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_gif.c"
    "${PXA_C_CORE_ROOT}/adapters/lvgl/pxa_lvgl_gif_native.c"
    "${PXA_C_CORE_ROOT}/adapters/esp/pxa_esp_rom_image.c"
    "${PXA_C_CORE_ROOT}/adapters/image/pxa_stream_png.c"
    "${PXA_C_CORE_ROOT}/adapters/image/vendor/pngle.c"
    "${PXA_C_CORE_ROOT}/adapters/image/vendor/miniz.c")
set(PXA_PRIV_REQUIRES
    mbedtls
    espressif__esp_audio_codec
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
set(PXA_RASTER_SPAN_IRAM_DEFAULT OFF)
if(PXA_BOARD STREQUAL "esp32s31-korvo-1" OR PXA_BOARD STREQUAL "doit-gamekit")
    # Keep the depth-texture pixel loop in internal instruction memory on
    # these S31 game boards. Other boards retain their mapped-code policy.
    set(PXA_RASTER_SPAN_IRAM_DEFAULT ON)
endif()
option(PXA_RASTER_SPAN_IRAM "Place the depth-texture pixel loop in IRAM"
       ${PXA_RASTER_SPAN_IRAM_DEFAULT})
set(PXA_LINKER_FRAGMENTS)
if(PXA_RASTER_SPAN_IRAM AND CONFIG_COMPILER_OPTIMIZATION_PERF AND CONFIG_IDF_TARGET_ESP32S31)
    list(APPEND PXA_LINKER_FRAGMENTS "raster_span.lf")
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
    LDFRAGMENTS ${PXA_LINKER_FRAGMENTS}
)
# Device benchmarks select O3 for the S31 span kernel and Watcher's native
# game renderer. Keep this local to the rasterizer and performance builds,
# without changing Guest/debug policy or unmeasured boards.
if(CONFIG_COMPILER_OPTIMIZATION_PERF AND
   (CONFIG_IDF_TARGET_ESP32S31 OR PXA_BOARD STREQUAL "sensecap-watcher"))
    set_source_files_properties(
        "${PXA_C_CORE_ROOT}/src/services/surface/raster.c"
        PROPERTIES COMPILE_OPTIONS "-O3")
endif()
if(CONFIG_WAMR_ENABLE_LIBC_WASI)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_WAMR_LIBC_WASI=1)
endif()
include("${PXSYS_ROOT}/cmake/PxaWamrMetadata.cmake")
pxsys_apply_wamr_metadata(${COMPONENT_LIB})

# Explicit diagnostic build only: release builds incur no audio tracing cost.
if(PXA_AUDIO_LATENCY_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_AUDIO_LATENCY_TRACE=1)
endif()

if(CONFIG_IDF_TARGET_ESP32S3 OR CONFIG_IDF_TARGET_ESP32S31)
    include("${PXSYS_ROOT}/platforms/esp-idf/cmake/pxsys_wamr.cmake")
    if(DEFINED PXA_HOST_SDK_ROOT)
        # Archives have verified provenance, but deliberately contain no .git.
        set(PXSYS_WAMR_OVERLAY_TOOL
            "${PXA_HOST_SDK_ROOT}/tools/host-sdk/prepare_wamr_overlay.py")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PXSYS_WAMR_OVERLAY_TOOL}")
    endif()
    pxsys_configure_wamr_esp_idf(__idf_wamr)
endif()
