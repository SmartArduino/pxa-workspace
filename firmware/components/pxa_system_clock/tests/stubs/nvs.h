#ifndef CLOCK_TEST_NVS_H
#define CLOCK_TEST_NVS_H

#include <stdint.h>
#include <esp_system.h>

typedef unsigned nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* handle);
esp_err_t nvs_get_i64(nvs_handle_t handle, const char* key, int64_t* value);
esp_err_t nvs_set_i64(nvs_handle_t handle, const char* key, int64_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#endif
