/* p4_wireless extension API. For WiFi / BLE use the standard esp_wifi and NimBLE APIs. */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wireless chip firmware version string */
esp_err_t p4_wireless_get_fw_version(char *buf, size_t len);

/* Current PHY rate in bps, STA must be connected; rx_rate is always 0 */
esp_err_t p4_wireless_get_data_rate(uint32_t *tx_rate, uint32_t *rx_rate);

/* Wireless chip temperature in Celsius. Not reliable yet, do not use for thermal protection. */
esp_err_t p4_wireless_get_temperature(int32_t *temp);

typedef void (*p4_wireless_hci_rx_cb_t)(uint8_t type, uint8_t *data, uint16_t len);

/* Used by src/nimble_shim.c, applications do not need to call these */
esp_err_t p4_wireless_hci_open(p4_wireless_hci_rx_cb_t rx_cb);

esp_err_t p4_wireless_hci_send(const uint8_t *h4, uint32_t len);

#ifdef __cplusplus
}
#endif
