// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>

/* A bulk transfer keeps the station awake while it can exchange data. */
bool pxa_esp_wifi_transfer_acquire(void);
void pxa_esp_wifi_transfer_release(void);
