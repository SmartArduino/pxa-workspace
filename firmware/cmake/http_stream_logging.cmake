# A short HTTPS read can return WANT_READ between fragments of one TLS record.
# IDF reports this expected retry as a warning for every fragment. Keep it at
# debug like its ordinary poll timeout, without hiding other HTTP diagnostics.
idf_component_get_property(pxa_http_dir esp_http_client COMPONENT_DIR)
idf_component_get_property(pxa_http_lib esp_http_client COMPONENT_LIB)
set(pxa_http_input "${pxa_http_dir}/esp_http_client.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pxa_http_input}")
file(READ "${pxa_http_input}" pxa_http_source)
set(pxa_http_log [=[ESP_LOG_LEVEL(sev, TAG, "esp_transport_read returned:%d and errno:%d ", rlen, errno);]=])
string(FIND "${pxa_http_source}" "${pxa_http_log}" pxa_http_log_at)
if(NOT pxa_http_log_at EQUAL -1)
    string(REPLACE "${pxa_http_log}" [=[if (client->timeout_ms <= 50 && rlen == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT &&
                    (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    sev = ESP_LOG_DEBUG;
                }
                ESP_LOG_LEVEL(sev, TAG, "esp_transport_read returned:%d and errno:%d ", rlen, errno);]=]
        pxa_http_source "${pxa_http_source}")
    # Streaming exchanges poll headers at 1 or 50 ms. EAGAIN asks the
    # worker to yield and retry until its request deadline expires.
    string(REPLACE [=[ESP_LOGW(TAG, "Connection timed out before data was ready!");]=]
        [=[ESP_LOG_LEVEL(client->timeout_ms <= 50 ? ESP_LOG_DEBUG : ESP_LOG_WARN,
                              TAG, "Connection timed out before data was ready!");]=]
        pxa_http_source "${pxa_http_source}")
    set(pxa_http_output "${CMAKE_CURRENT_BINARY_DIR}/pxa-idf-http/esp_http_client.c")
    get_target_property(pxa_http_sources ${pxa_http_lib} SOURCES)
    list(FILTER pxa_http_sources EXCLUDE REGEX "(^|/)esp_http_client[.]c$")
    list(APPEND pxa_http_sources "${pxa_http_output}")
    file(GENERATE OUTPUT "${pxa_http_output}" CONTENT "${pxa_http_source}")
    set_property(TARGET ${pxa_http_lib} PROPERTY SOURCES "${pxa_http_sources}")
    target_include_directories(${pxa_http_lib} PRIVATE "${pxa_http_dir}")
endif()
