// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_peripherals.h>

// Register one application-owned indicator in org.doit.peripherals. Driver I/O
// runs on a worker, never on the Host request or resource I/O path. Initialize
// once after the board's strip driver; the driver must outlive the service.
bool pxa_board_rgb_port_initialize(void* context,
    bool (*apply)(void*, unsigned red, unsigned green, unsigned blue));

// A single-color board indicator uses GPIO-kind logical on/off, not a fake RGB
// capability. It has the same worker ownership and cleanup guarantees.
bool pxa_board_binary_indicator_initialize(void* context, bool (*apply)(void*, bool on));
