#pragma once
#include "pxa/haptics.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Startup only; backend and context have board lifetime. */
bool pxa_esp_haptics_set_backend(const pxa_haptics_backend_t *);
const pxa_haptics_backend_t *pxa_esp_haptics_backend(void);
#ifdef __cplusplus
}
#endif
