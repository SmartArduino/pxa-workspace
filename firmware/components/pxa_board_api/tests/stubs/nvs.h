#pragma once
#include <stdint.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
enum { ESP_OK = 0, NVS_READONLY = 0, NVS_READWRITE = 1 };
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
