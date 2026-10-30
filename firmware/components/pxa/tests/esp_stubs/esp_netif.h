#pragma once
#include "pxa_test_platform.h"
typedef struct esp_netif { int up; uint32_t address; } esp_netif_t;
typedef struct { struct { uint32_t addr; } ip; } esp_netif_ip_info_t;
esp_netif_t *esp_netif_get_default_netif(void);
bool esp_netif_is_netif_up(esp_netif_t *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
