#ifndef PXA_ESP_SENSOR_H
#define PXA_ESP_SENSOR_H
#include "pxa/sensor.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Board startup only, before Host initialization. Descriptors and provider
 * context must live for the device lifetime. Runtime owns permission checks,
 * subscription limits, scheduling and event delivery. NULL clears provider. */
void pxa_esp_sensor_set_provider(const pxa_sensor_config_t *provider);
void pxa_esp_sensor_configure(pxa_sensor_config_t *config);
#ifdef __cplusplus
}
#endif
#endif
