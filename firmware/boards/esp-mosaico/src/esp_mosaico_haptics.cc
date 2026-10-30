#include "esp_mosaico_haptics.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <pxa/pxa_esp_haptics.h>

namespace {
constexpr auto kMotor = GPIO_NUM_8;
SemaphoreHandle_t g_mutex = nullptr;
esp_timer_handle_t g_timer = nullptr;
int64_t g_deadline_us = 0;

void Expire(void*) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    const int64_t remaining = g_deadline_us - esp_timer_get_time();
    // A previous expiry may already have been queued when a new pulse starts.
    if (g_deadline_us && remaining > 0) {
        (void)esp_timer_stop(g_timer);
        if (esp_timer_start_once(g_timer, remaining) != ESP_OK) {
            (void)gpio_set_level(kMotor, 0);
            g_deadline_us = 0;
        }
    } else {
        (void)gpio_set_level(kMotor, 0);
        g_deadline_us = 0;
    }
    xSemaphoreGive(g_mutex);
}
pxa_status_t Play(void*, uint32_t duration, uint16_t amplitude) {
    if (amplitude != 1000) return PXA_STATUS_UNSUPPORTED;
    const auto error = EspMosaicoVibrate(duration);
    if (error == ESP_OK) return PXA_STATUS_OK;
    return error == ESP_ERR_INVALID_ARG ? PXA_STATUS_INVALID_ARGUMENT : PXA_STATUS_IO_ERROR;
}
}

bool EspMosaicoInitializeHaptics() {
    if (g_timer) return true;
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << kMotor;
    config.mode = GPIO_MODE_OUTPUT;
    if (gpio_set_level(kMotor, 0) != ESP_OK || gpio_config(&config) != ESP_OK) return false;
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) return false;
    esp_timer_create_args_t timer = {};
    timer.callback = Expire;
    timer.name = "mosaico_motor";
    if (esp_timer_create(&timer, &g_timer) != ESP_OK) {
        vSemaphoreDelete(g_mutex); g_mutex = nullptr;
        return false;
    }
    pxa_haptics_backend_t backend = {};
    backend.struct_size = sizeof(backend);
    backend.flags = PXA_HAPTICS_CAP_VIBRATE;
    backend.max_duration_ms = 1000;
    backend.play = Play;
    backend.stop = [](void*) { (void)EspMosaicoVibrate(0); };
    return pxa_esp_haptics_set_backend(&backend);
}

esp_err_t EspMosaicoVibrate(uint32_t duration_ms) {
    if (duration_ms > 1000) return ESP_ERR_INVALID_ARG;
    if (!g_timer) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    (void)esp_timer_stop(g_timer);
    g_deadline_us = 0;
    esp_err_t result = gpio_set_level(kMotor, 0);
    if (result == ESP_OK && duration_ms) {
        g_deadline_us = esp_timer_get_time() + int64_t(duration_ms) * 1000;
        result = esp_timer_start_once(g_timer, uint64_t(duration_ms) * 1000);
        if (result == ESP_OK) result = gpio_set_level(kMotor, 1);
        if (result != ESP_OK) {
            (void)esp_timer_stop(g_timer);
            (void)gpio_set_level(kMotor, 0);
            g_deadline_us = 0;
        }
    }
    xSemaphoreGive(g_mutex);
    return result;
}

