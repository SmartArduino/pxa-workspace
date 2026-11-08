#include "watcher_sensors.h"
#include "watcher_battery.h"
#include "pxa/pxa_esp_sensor.h"

#include <cmath>
#include <cstring>
#include <driver/temperature_sensor.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "WatcherSensors";
temperature_sensor_handle_t temperature;
WatcherBattery* battery_monitor;
struct Channel { uint16_t id, users; } channels[] = {{1, 0}, {2, 0}};
pxa_sensor_descriptor_t descriptors[2];
uint16_t count;

Channel* Find(uint16_t id) {
    if (id == 1 && temperature) return &channels[0];
    if (id == 2 && battery_monitor) return &channels[1];
    return nullptr;
}
pxa_status_t Subscribe(void*, uint16_t id, uint32_t, void** subscription) {
    if (!subscription) return PXA_STATUS_INVALID_ARGUMENT;
    *subscription = nullptr;
    auto* channel = Find(id);
    if (!channel) return PXA_STATUS_NOT_FOUND;
    if (channel->users == UINT16_MAX) return PXA_STATUS_RESOURCE_LIMIT;
    if (id == 1 && !channel->users &&
        temperature_sensor_enable(temperature) != ESP_OK)
        return PXA_STATUS_UNAVAILABLE;
    ++channel->users;
    *subscription = channel;
    return PXA_STATUS_OK;
}
void Unsubscribe(void*, void* subscription, uint16_t id) {
    auto* channel = Find(id);
    if (!channel || subscription != channel || !channel->users) return;
    if (!--channel->users && id == 1)
        (void)temperature_sensor_disable(temperature);
}
pxa_status_t Read(void*, void* subscription, uint16_t id, int32_t values[3]) {
    auto* channel = Find(id);
    if (!channel || subscription != channel || !channel->users)
        return PXA_STATUS_NOT_FOUND;
    values[0] = values[1] = values[2] = 0;
    if (id == 1) {
        float celsius = 0;
        if (temperature_sensor_get_celsius(temperature, &celsius) != ESP_OK ||
            !std::isfinite(celsius) || celsius < -10 || celsius > 80)
            return PXA_STATUS_UNAVAILABLE;
        values[0] = static_cast<int32_t>(std::lround(celsius * 1000));
    } else {
        uint16_t mv = 0;
        // The legacy IsPresent bit reflects external power on this board;
        // it can be false with a connected cell while USB is supplying power.
        if (!battery_monitor->GetVoltageMv(&mv))
            return PXA_STATUS_UNAVAILABLE;
        values[0] = static_cast<int32_t>(mv) * 1000;
    }
    return PXA_STATUS_OK;
}
void Describe(uint16_t id, const char* semantic, uint16_t unit, uint32_t period) {
    descriptors[count++] = {id,
        {reinterpret_cast<const uint8_t*>(semantic), std::strlen(semantic)},
        unit, 1, period, 60000};
}
}  // namespace

void WatcherInitializeSensors(WatcherBattery* battery) {
    // This is the ESP32 die temperature, not an ambient-temperature sensor.
    const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&config, &temperature) == ESP_OK)
        Describe(1, "chip-temperature", PXA_SENSOR_UNIT_MILLI_CELSIUS, 1000);
    else
        ESP_LOGW(kTag, "Chip temperature sensor unavailable");
    battery_monitor = battery;
    if (battery_monitor)
        Describe(2, "battery-voltage", PXA_SENSOR_UNIT_MICRO_VOLTS, 2000);
    pxa_sensor_config_t provider{};
    provider.struct_size = sizeof(provider);
    provider.descriptors = descriptors;
    provider.descriptor_count = count;
    provider.subscribe = Subscribe;
    provider.read = Read;
    provider.unsubscribe = Unsubscribe;
    pxa_esp_sensor_set_provider(&provider);
    ESP_LOGI(kTag, "%u physical sensor channels registered", count);
}
