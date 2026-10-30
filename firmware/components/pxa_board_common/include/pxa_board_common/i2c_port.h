// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <driver/i2c_master.h>
#include <cstddef>
#include <cstdint>

// One external port on an existing board-owned bus. Internal chip addresses
// must be denied; pins, bus lifetime and power rails remain board-owned.
bool pxa_board_i2c_port_initialize(i2c_master_bus_handle_t bus, uint16_t port,
    const char* scope, const uint8_t* denied_addresses, size_t denied_count);
