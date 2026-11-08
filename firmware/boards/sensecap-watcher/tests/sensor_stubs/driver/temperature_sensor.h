#pragma once
#include <esp_err.h>
struct TestTemperature { bool enabled; };
using temperature_sensor_handle_t = TestTemperature*;
struct temperature_sensor_config_t { int low, high; };
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(low, high) {low, high}
esp_err_t temperature_sensor_install(const temperature_sensor_config_t*, temperature_sensor_handle_t*);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t);
esp_err_t temperature_sensor_disable(temperature_sensor_handle_t);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t, float*);
