#pragma once
#include <driver/i2c_master.h>
#include "esp_mosaico_battery.h"

// Optional devices are probed independently; only working chips are exposed.
// Call once after creating the board's shared I2C bus, before Host startup.
void EspMosaicoInitializeSensors(i2c_master_bus_handle_t bus, const EspMosaicoBattery* battery = nullptr);
