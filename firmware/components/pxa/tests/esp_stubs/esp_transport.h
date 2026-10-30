#pragma once
#include "pxa_test_platform.h"
typedef void* esp_transport_handle_t;
esp_transport_handle_t esp_transport_tcp_init(void);
esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t, void (*)(void));
int esp_transport_connect(esp_transport_handle_t, const char*, int, int);
int esp_transport_write(esp_transport_handle_t, const char*, int, int);
int esp_transport_poll_read(esp_transport_handle_t, int);
int esp_transport_read(esp_transport_handle_t, char*, int, int);
int esp_transport_close(esp_transport_handle_t);
int esp_transport_destroy(esp_transport_handle_t);
