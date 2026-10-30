#pragma once
#include <driver/i2c_master.h>
#include <cstdint>

// Call after power-up and revision detection. Owns the external bus, EEPROM
// discovery and native resources behind the optional peripherals extension.
bool EspMosaicoInitializePeripherals(i2c_master_bus_handle_t onboard_bus,
    uint16_t revision, bool usb_console_ready);
bool EspMosaicoStopPeripherals();
