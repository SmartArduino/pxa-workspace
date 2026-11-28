// SPDX-License-Identifier: Apache-2.0
#include "pxa_esp_wifi_transfer.h"
#include <esp_wifi.h>
#include <pthread.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned users;
static wifi_ps_type_t previous;

bool pxa_esp_wifi_transfer_acquire(void) {
    pthread_mutex_lock(&lock);
    const bool acquired = users || (esp_wifi_get_ps(&previous) == ESP_OK &&
        (previous == WIFI_PS_NONE || esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK));
    if (acquired) ++users;
    pthread_mutex_unlock(&lock);
    return acquired;
}

void pxa_esp_wifi_transfer_release(void) {
    pthread_mutex_lock(&lock);
    wifi_ps_type_t current;
    if (users && !--users && previous != WIFI_PS_NONE &&
        esp_wifi_get_ps(&current) == ESP_OK && current == WIFI_PS_NONE)
        (void)esp_wifi_set_ps(previous);
    pthread_mutex_unlock(&lock);
}
