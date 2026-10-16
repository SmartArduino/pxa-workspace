idf_component_register(SRCS "benchmark.c" INCLUDE_DIRS "." "${JPEG_FIXTURE_DIR}"
    REQUIRES esp_new_jpeg esp_timer esp_psram)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
if(PXA_IMAGE_BASELINE_FILE)
    set(core "${PXA_ROOT}/deps/pxa-system/libpxa")
    set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/image_baseline.c")
    file(WRITE "${wrapper}" "")
    foreach(name target_options target_dimensions target_begin target_blit
                 decoder_create decoder_write decoder_finish decoder_destroy)
        file(APPEND "${wrapper}" "#define pxa_image_${name} before_pxa_image_${name}\n")
    endforeach()
    file(APPEND "${wrapper}" "#include \"${PXA_IMAGE_BASELINE_FILE}\"\n")
    target_sources(${COMPONENT_LIB} PRIVATE "blit_benchmark.c" "${wrapper}"
        "${core}/src/services/assets/image/image_decode.c"
        "${core}/src/services/assets/asset_object.c"
        "${core}/src/services/assets/resource_budget.c"
        "${core}/src/core/wire.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${core}/include")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_BLIT_BENCHMARK=1)
endif()
