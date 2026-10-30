#pragma once
#include <pxa_ext/peripherals.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
bool pxa_esp_peripherals_set_backend(const pxa_peripherals_backend_t *);
const pxa_peripherals_backend_t *pxa_esp_peripherals_backend(void);
#ifdef __cplusplus
}
#endif
