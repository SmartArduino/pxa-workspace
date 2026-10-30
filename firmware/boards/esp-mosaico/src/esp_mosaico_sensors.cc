#include "esp_mosaico_sensors.h"
#include "esp_mosaico_sensor_units.h"
#include "esp_mosaico_sensor_frame.h"

#include <array>
#include <cstring>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pxa/pxa_esp_sensor.h>
#include "bmi270.h"
#include "bmm150.h"

namespace {
constexpr char kTag[] = "mosaico_sensors";
struct Device { i2c_master_dev_handle_t i2c = nullptr; uint16_t users = 0; int64_t ready_us = 0; };
Device g_imu_bus, g_mag_bus[2];
bmi2_dev g_imu = {};
bmm150_dev g_mag[2] = {};
pxa_sensor_descriptor_t g_descriptors[8] = {};
const EspMosaicoBattery* g_battery = nullptr;
Device g_battery_subscriptions;
uint16_t g_descriptor_count = 0;
bool g_imu_present = false, g_mag_present[2] = {};
constexpr uint8_t kImuSensors[] = {BMI2_ACCEL, BMI2_GYRO};

int8_t Read(uint8_t reg, uint8_t* data, uint32_t bytes, void* context) {
    auto* device = static_cast<Device*>(context);
    return i2c_master_transmit_receive(device->i2c, &reg, 1, data, bytes, 20) == ESP_OK ? 0 : -1;
}
int8_t Write(uint8_t reg, const uint8_t* data, uint32_t bytes, void* context) {
    if (bytes > 128) return -1;
    auto* device = static_cast<Device*>(context);
    std::array<uint8_t, 129> packet;
    packet[0] = reg;
    std::memcpy(packet.data() + 1, data, bytes);
    return i2c_master_transmit(device->i2c, packet.data(), bytes + 1, 20) == ESP_OK ? 0 : -1;
}
void Delay(uint32_t us, void*) {
    if (us >= 1000) vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
    else esp_rom_delay_us(us);
}
bool AddDevice(i2c_master_bus_handle_t bus, uint8_t address, Device* device) {
    if (i2c_master_probe(bus, address, 20) != ESP_OK) return false;
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(bus, &config, &device->i2c) == ESP_OK;
}
void Describe(uint16_t id, const char* semantic, uint16_t unit, uint8_t dimensions, uint32_t period) {
    auto& descriptor = g_descriptors[g_descriptor_count++];
    descriptor.id = id;
    descriptor.semantic = {reinterpret_cast<const uint8_t*>(semantic), std::strlen(semantic)};
    descriptor.unit = unit;
    descriptor.dimensions = dimensions;
    descriptor.min_period_ms = period;
    descriptor.max_period_ms = 60000;
}
Device* Find(uint16_t id) {
    if (id >= 1 && id <= 3) return g_imu_present ? &g_imu_bus : nullptr;
    if (id >= 4 && id <= 5) return g_mag_present[id - 4] ? &g_mag_bus[id - 4] : nullptr;
    if (id >= 6 && id <= 8) return g_battery ? &g_battery_subscriptions : nullptr;
    return nullptr;
}
pxa_status_t Subscribe(void*, uint16_t id, uint32_t, void** subscription) {
    if (!subscription) return PXA_STATUS_INVALID_ARGUMENT;
    *subscription = nullptr;
    auto* device = Find(id);
    if (!device) return PXA_STATUS_NOT_FOUND;
    if (!device->users && id <= 5) {
        int8_t result;
        if (id <= 3) result = bmi270_sensor_enable(kImuSensors, 2, &g_imu);
        else {
            bmm150_settings settings = {};
            settings.pwr_mode = BMM150_POWERMODE_NORMAL;
            result = bmm150_set_op_mode(&settings, &g_mag[id - 4]);
        }
        if (result != 0) return PXA_STATUS_IO_ERROR;
        device->ready_us = esp_timer_get_time() + (id <= 3 ? 80000 : 100000);
    }
    ++device->users;
    *subscription = device;
    return PXA_STATUS_OK;
}
void Unsubscribe(void*, void* subscription, uint16_t id) {
    auto* device = Find(id);
    if (!device || subscription != device || !device->users || --device->users) return;
    if (id >= 6) return;
    if (id <= 3) (void)bmi270_sensor_disable(kImuSensors, 2, &g_imu);
    else {
        bmm150_settings settings = {};
        settings.pwr_mode = BMM150_POWERMODE_SLEEP;
        (void)bmm150_set_op_mode(&settings, &g_mag[id - 4]);
    }
}
pxa_status_t Sample(void*, void* subscription, uint16_t id, int32_t values[3]) {
    auto* device = Find(id);
    if (!device || subscription != device || !device->users) return PXA_STATUS_NOT_FOUND;
    if (esp_timer_get_time() < device->ready_us) return PXA_STATUS_UNAVAILABLE;
    values[0] = values[1] = values[2] = 0;
    if (id >= 6) {
        const auto battery = g_battery->Get();
        if (!battery.valid || (id == 8 && !battery.temperature_valid)) return PXA_STATUS_UNAVAILABLE;
        if (id == 6) values[0] = int32_t(battery.voltage_mv) * 1000;
        if (id == 7) values[0] = int32_t(battery.current_ma) * 1000;
        if (id == 8) values[0] = battery.temperature_milli_celsius;
    } else if (id == 3) {
        int16_t temperature;
        if (bmi2_get_temperature_data(&temperature, &g_imu) != BMI2_OK || temperature == INT16_MIN)
            return PXA_STATUS_IO_ERROR;
        values[0] = mosaico_board::TemperatureMilli(temperature);
    } else if (id <= 2) {
        bmi2_sens_data data = {};
        if (bmi2_get_sensor_data(&data, &g_imu) != BMI2_OK) return PXA_STATUS_IO_ERROR;
        const auto& axes = id == 1 ? data.acc : data.gyr;
        const auto scale = id == 1 ? mosaico_board::AccelerationMicro : mosaico_board::AngularVelocityMicro;
        const auto device_axes = mosaico_board::ImuDeviceFrame(
            scale(axes.x), scale(axes.y), scale(axes.z));
        std::memcpy(values, device_axes.data(), sizeof(int32_t) * device_axes.size());
    } else {
        bmm150_mag_data data = {};
        if (bmm150_read_mag_data(&data, &g_mag[id - 4]) != BMM150_OK ||
            data.x == BMM150_OVERFLOW_OUTPUT || data.y == BMM150_OVERFLOW_OUTPUT ||
            data.z == BMM150_OVERFLOW_OUTPUT) return PXA_STATUS_IO_ERROR;
        values[0] = mosaico_board::MagneticNano(data.x);
        values[1] = mosaico_board::MagneticNano(data.y);
        values[2] = mosaico_board::MagneticNano(data.z);
    }
    return PXA_STATUS_OK;
}
}

void EspMosaicoInitializeSensors(i2c_master_bus_handle_t bus, const EspMosaicoBattery* battery) {
    if (AddDevice(bus, 0x69, &g_imu_bus)) {
        g_imu.intf = BMI2_I2C_INTF;
        g_imu.intf_ptr = &g_imu_bus;
        g_imu.read = Read; g_imu.write = Write; g_imu.delay_us = Delay;
        g_imu.read_write_len = 128;
        int8_t result = bmi270_init(&g_imu);
        bmi2_sens_config config[2] = {};
        config[0].type = BMI2_ACCEL; config[1].type = BMI2_GYRO;
        if (result == BMI2_OK) result = bmi270_get_sensor_config(config, 2, &g_imu);
        config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
        config[0].cfg.acc.range = BMI2_ACC_RANGE_4G;
        config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
        config[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
        if (result == BMI2_OK) result = bmi270_set_sensor_config(config, 2, &g_imu);
        if (result == BMI2_OK) result = bmi270_sensor_disable(kImuSensors, 2, &g_imu);
        g_imu_present = result == BMI2_OK;
        ESP_LOGI(kTag, "BMI270 at 0x69: %d", result);
        if (g_imu_present) {
            Describe(1, "motion.acceleration", PXA_SENSOR_UNIT_MICRO_METERS_PER_SECOND_SQUARED, 3, 10);
            Describe(2, "motion.angular-velocity", PXA_SENSOR_UNIT_MICRO_RADIANS_PER_SECOND, 3, 10);
            Describe(3, "imu-temperature", PXA_SENSOR_UNIT_MILLI_CELSIUS, 1, 100);
        } else { (void)i2c_master_bus_rm_device(g_imu_bus.i2c); g_imu_bus.i2c = nullptr; }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!AddDevice(bus, 0x11 + i, &g_mag_bus[i])) continue;
        auto& magnetometer = g_mag[i];
        magnetometer.intf = BMM150_I2C_INTF;
        magnetometer.intf_ptr = &g_mag_bus[i];
        magnetometer.read = Read; magnetometer.write = Write; magnetometer.delay_us = Delay;
        int8_t result = bmm150_init(&magnetometer);
        // This upstream API can return OK for an unexpected chip ID. Verify
        // the ID it actually accepted before configuring or advertising it.
        if (result == BMM150_OK && magnetometer.chip_id != BMM150_CHIP_ID)
            result = BMM150_E_DEV_NOT_FOUND;
        bmm150_settings settings = {};
        settings.preset_mode = BMM150_PRESETMODE_REGULAR;
        if (result == BMM150_OK) result = bmm150_set_presetmode(&settings, &magnetometer);
        settings.pwr_mode = BMM150_POWERMODE_SLEEP;
        if (result == BMM150_OK) result = bmm150_set_op_mode(&settings, &magnetometer);
        g_mag_present[i] = result == BMM150_OK;
        ESP_LOGI(kTag, "BMM150 at 0x%02x: %d", 0x11 + i, result);
        if (g_mag_present[i])
            Describe(4 + i, i == 0 ? "magnetic-field.0" : "magnetic-field.1", PXA_SENSOR_UNIT_NANO_TESLA, 3, 100);
        else { (void)i2c_master_bus_rm_device(g_mag_bus[i].i2c); g_mag_bus[i].i2c = nullptr; }
    }
    if (battery && i2c_master_probe(bus, 0x55, 20) == ESP_OK) {
        g_battery = battery;
        Describe(6, "battery-voltage", PXA_SENSOR_UNIT_MICRO_VOLTS, 1, 2000);
        Describe(7, "battery-current", PXA_SENSOR_UNIT_MICRO_AMPERES, 1, 2000);
        Describe(8, "battery-temperature", PXA_SENSOR_UNIT_MILLI_CELSIUS, 1, 2000);
    }
    pxa_sensor_config_t provider = {};
    provider.struct_size = sizeof(provider);
    provider.descriptors = g_descriptors;
    provider.descriptor_count = g_descriptor_count;
    provider.subscribe = Subscribe; provider.read = Sample; provider.unsubscribe = Unsubscribe;
    pxa_esp_sensor_set_provider(&provider);
    ESP_LOGI(kTag, "%u sensor channels registered", g_descriptor_count);
}
