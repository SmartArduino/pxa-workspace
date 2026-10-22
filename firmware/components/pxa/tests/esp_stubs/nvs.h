#ifndef PXA_TEST_NVS_H
#define PXA_TEST_NVS_H
#include "pxa_test_platform.h"
typedef unsigned nvs_handle_t;
enum {NVS_READONLY,NVS_READWRITE};
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
void nvs_close(nvs_handle_t);
#endif
