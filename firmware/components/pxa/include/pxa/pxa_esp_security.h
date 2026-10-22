#ifndef PXA_ESP_SECURITY_H
#define PXA_ESP_SECURITY_H
#include "pxa/status.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Standalone product bootstrap only: after NVS, BEFORE board registration or
 * any RF, ADC or I2S initialization. Existing key loads without entropy setup;
 * first boot temporarily enables the bootloader hardware entropy source. */
pxa_status_t pxa_esp_security_initialize_early(void);
#ifdef __cplusplus
}
#endif
#endif
