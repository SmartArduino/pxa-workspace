idf_component_register(SRCS main.cc ../../sd_device_test.cc ../../../src/espp4_a_sdio.cc
    INCLUDE_DIRS ../../../include
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_driver_sdmmc fatfs sdmmc esp_timer esp_hw_support)
option(PXA_SD_PROBE_INTERNAL "Use internal DMA buffers instead of PSRAM" OFF)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PXA_ESPP4_SD_TEST_FORCE_INTERNAL=$<BOOL:${PXA_SD_PROBE_INTERNAL}>)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_read_sectors" "-Wl,--wrap=sdmmc_write_sectors")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_card_init" "-Wl,-u,__wrap_sdmmc_card_init")
