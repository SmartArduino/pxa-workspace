# p4_wireless (prebuilt). Do not modify, see README.md for usage.
set(_srcs "src/sdmmc_shim.c")
if(CONFIG_BT_NIMBLE_ENABLED AND CONFIG_BT_CONTROLLER_DISABLED)
    list(APPEND _srcs "src/nimble_shim.c")
endif()

set(_inc "include")
if(IDF_VERSION_MAJOR EQUAL 5 OR (IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 0))
    list(APPEND _inc "include_compat")
endif()

idf_component_register(
    SRCS ${_srcs}
    INCLUDE_DIRS ${_inc}
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_event esp_netif esp_wifi
        freertos sdmmc esp_driver_sdmmc esp_driver_gpio heap lwip esp_pm driver
        esp_hw_support esp_security hal log
        bt
)

if(CONFIG_BT_NIMBLE_ENABLED AND CONFIG_BT_CONTROLLER_DISABLED)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ble_transport_ll_init")
endif()

add_prebuilt_library(p4_wireless_prebuilt "${CMAKE_CURRENT_LIST_DIR}/lib/libp4_wireless.a"
    PRIV_REQUIRES freertos sdmmc esp_driver_sdmmc esp_driver_gpio heap lwip
                  esp_pm driver esp_hw_support esp_security hal esp_netif esp_event esp_wifi
                  ${COMPONENT_NAME})
target_link_libraries(${COMPONENT_LIB} PUBLIC p4_wireless_prebuilt)
target_link_libraries(${COMPONENT_LIB} INTERFACE
    -Wl,--undefined=p4_wireless_nvram_info
    -Wl,--undefined=p4_wireless_userconfig_info
    -Wl,--undefined=p4_wireless_chip_desc_lookup)

idf_component_get_property(_wifi_lib esp_wifi COMPONENT_LIB)
target_link_libraries(${_wifi_lib} PUBLIC ${COMPONENT_LIB})

# Version and config checks, these must match the prebuilt library
set(_cur_idf "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(_cur_idf VERSION_LESS "5.5" OR NOT _cur_idf VERSION_LESS "7.0")
    message(FATAL_ERROR "p4_wireless supports ESP-IDF 5 (5.5 or later) and ESP-IDF 6, current is v${_cur_idf}.")
endif()
set(_verified 5.5.3 6.0.1 6.2.0)
if(NOT _cur_idf IN_LIST _verified)
    message(WARNING "p4_wireless: verified ESP-IDF versions are ${_verified}, current v${_cur_idf} is not verified.")
endif()
if(NOT CONFIG_FREERTOS_HZ EQUAL 1000)
    message(FATAL_ERROR "p4_wireless requires CONFIG_FREERTOS_HZ=1000, current is ${CONFIG_FREERTOS_HZ}. Change it in menuconfig.")
endif()
if(NOT CONFIG_LWIP_IPV6)
    message(FATAL_ERROR "p4_wireless requires CONFIG_LWIP_IPV6=y (lwIP IPv6), currently disabled. Enable it in menuconfig.")
endif()
if(IDF_VERSION_MAJOR EQUAL 6 AND NOT CONFIG_LIBC_NEWLIB)
    message(FATAL_ERROR "p4_wireless requires CONFIG_LIBC_NEWLIB=y (newlib C library) on IDF 6. Change it in menuconfig.")
endif()
if(CONFIG_ESP_WIFI_REMOTE_IN_IDF AND NOT CONFIG_SLAVE_IDF_TARGET_ESP32C6)
    message(FATAL_ERROR "p4_wireless requires the Wi-Fi Remote slave target to be ESP32C6 (default). Restore it in menuconfig.")
endif()
