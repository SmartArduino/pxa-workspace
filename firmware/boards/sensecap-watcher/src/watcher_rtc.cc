// SPDX-License-Identifier: MIT
#include "watcher_rtc_calendar.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <pxa_system_clock.h>

namespace {
i2c_master_dev_handle_t device;
bool Read(void*, int64_t* seconds) {
    uint8_t reg = 0, control = 0, raw[7];
    if (i2c_master_transmit_receive(device, &reg, 1, &control, 1, 30) != ESP_OK || (control & 0xa0)) return false;
    reg = 2;
    return i2c_master_transmit_receive(device, &reg, 1, raw, sizeof(raw), 30) == ESP_OK &&
           watcher_rtc::Decode(raw, seconds);
}
bool Write(void*, int64_t seconds) {
    uint8_t raw[7];
    if (!watcher_rtc::Encode(seconds, raw)) return false;
    uint8_t output[8] = {2};
    for (unsigned i = 0; i < 7; ++i) output[i + 1] = raw[i];
    // One transaction freezes all calendar counters; preserve alarms/CLKOUT.
    if (i2c_master_transmit(device, output, sizeof(output), 30) != ESP_OK) return false;
    const uint8_t reg = 0;
    uint8_t control = 0;
    if (i2c_master_transmit_receive(device, &reg, 1, &control, 1, 30) != ESP_OK) return false;
    const uint8_t start[] = {0, uint8_t(control & ~0xa0u)}; // release STOP/test mode
    return i2c_master_transmit(device, start, sizeof(start), 30) == ESP_OK;
}
}

void WatcherInitializeRtc(i2c_master_bus_handle_t bus) {
    if (device || i2c_master_probe(bus, 0x51, 30) != ESP_OK) return;
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x51, .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &config, &device) != ESP_OK) return;
    const pxa_system_clock_source_t source = {
        .struct_size = sizeof(source), .context = nullptr, .read_utc = Read, .write_utc = Write,
    };
    if (!pxa_system_clock_set_source(&source)) {
        (void)i2c_master_bus_rm_device(device); device = nullptr;
        ESP_LOGW("watcher_rtc", "RTC registration unavailable; using system clock fallback");
    } else ESP_LOGI("watcher_rtc", "PCF8563 registered as retained UTC clock");
}
