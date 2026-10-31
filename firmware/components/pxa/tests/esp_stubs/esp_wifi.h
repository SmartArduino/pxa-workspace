#ifndef PXA_TEST_ESP_WIFI_H
#define PXA_TEST_ESP_WIFI_H
#include "pxa_test_platform.h"
typedef enum {WIFI_PS_NONE,WIFI_PS_MIN_MODEM,WIFI_PS_MAX_MODEM} wifi_ps_type_t;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
#endif
