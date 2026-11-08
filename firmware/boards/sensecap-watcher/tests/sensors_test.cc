// SPDX-License-Identifier: Apache-2.0
#include "../src/watcher_sensors.cc"
#include <cassert>
#include <cstdio>
#include <limits>
static TestTemperature chip{};
static bool missing, failed;
static float sample = 50.5f;
static unsigned enabled, disabled;
esp_err_t temperature_sensor_install(const temperature_sensor_config_t* config, temperature_sensor_handle_t* output) {
    assert(config->low == -10 && config->high == 80);
    *output = missing ? nullptr : &chip; return missing ? ESP_FAIL : ESP_OK;
}
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t handle) { assert(handle == &chip); handle->enabled = true; ++enabled; return ESP_OK; }
esp_err_t temperature_sensor_disable(temperature_sensor_handle_t handle) { assert(handle == &chip); handle->enabled = false; ++disabled; return ESP_OK; }
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t handle, float* output) { assert(handle->enabled); *output = sample; return failed ? ESP_FAIL : ESP_OK; }
int main(int argc, char**) {
    missing = argc > 1;
    WatcherBattery battery;
    WatcherInitializeSensors(missing ? nullptr : &battery);
    pxa_sensor_config_t config{}; config.struct_size = sizeof(config); config.max_subscriptions = 7;
    pxa_esp_sensor_configure(&config);
    if (missing) { assert(!config.descriptor_count && !config.read); puts("Watcher absent sensors expose no channels"); return 0; }
    assert(config.descriptor_count == 2 && config.max_subscriptions == 7);
    assert(config.descriptors[0].min_period_ms == 1000 && config.descriptors[1].min_period_ms == 2000);
    for (unsigned i = 0; i < 2; ++i) {
        auto& d = config.descriptors[i];
        assert(d.dimensions == 1 && d.max_period_ms == 60000);
        const char* expected = i ? "battery-voltage" : "chip-temperature";
        assert(d.semantic.size == std::strlen(expected) && !std::memcmp(d.semantic.data, expected, d.semantic.size));
        assert(d.unit == (i ? PXA_SENSOR_UNIT_MICRO_VOLTS : PXA_SENSOR_UNIT_MILLI_CELSIUS));
    }
    void *a = nullptr, *b = nullptr, *v = nullptr; int32_t values[3];
    assert(config.subscribe(nullptr, 3, 1000, &a) == PXA_STATUS_NOT_FOUND); // No invented IMU.
    assert(config.subscribe(nullptr, 1, 1000, &a) == PXA_STATUS_OK);
    assert(config.subscribe(nullptr, 1, 1000, &b) == PXA_STATUS_OK && a == b && enabled == 1);
    assert(config.read(nullptr, a, 1, values) == PXA_STATUS_OK && values[0] == 50500 && !values[1] && !values[2]);
    sample = std::numeric_limits<float>::quiet_NaN();
    assert(config.read(nullptr, a, 1, values) == PXA_STATUS_UNAVAILABLE);
    sample = 81; assert(config.read(nullptr, a, 1, values) == PXA_STATUS_UNAVAILABLE);
    failed = true; assert(config.read(nullptr, a, 1, values) == PXA_STATUS_UNAVAILABLE);
    config.unsubscribe(nullptr, a, 1); assert(chip.enabled && !disabled);
    config.unsubscribe(nullptr, b, 1); assert(!chip.enabled && disabled == 1);
    config.unsubscribe(nullptr, b, 1); assert(disabled == 1);
    assert(config.read(nullptr, a, 1, values) == PXA_STATUS_NOT_FOUND);
    assert(config.subscribe(nullptr, 2, 2000, &v) == PXA_STATUS_OK);
    assert(config.read(nullptr, v, 2, values) == PXA_STATUS_OK && values[0] == 3999000);
    battery.present = false; // External power does not invalidate the ADC sample.
    assert(config.read(nullptr, v, 2, values) == PXA_STATUS_OK && values[0] == 3999000);
    battery.present = true; battery.read_ok = false; assert(config.read(nullptr, v, 2, values) == PXA_STATUS_UNAVAILABLE);
    config.unsubscribe(nullptr, v, 2);
    puts("Watcher Sensor: physical capabilities, units, failures and subscription lifetime passed");
}
