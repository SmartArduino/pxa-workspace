idf_component_register(SRCS "main.c" "../kernels.c"
    INCLUDE_DIRS ".." REQUIRES wamr esp_timer esp_psram)
if(NOT EXISTS "${BENCH_AOT}")
    message(FATAL_ERROR "Set BENCH_AOT to the pinned compiler's benchmark.aot")
endif()
target_add_binary_data(${COMPONENT_LIB} "${BENCH_AOT}" BINARY RENAME_TO bench_aot)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffp-contract=off -fno-builtin)
