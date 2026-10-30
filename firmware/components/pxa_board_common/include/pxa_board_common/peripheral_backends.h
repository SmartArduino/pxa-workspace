// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/pxa_peripherals.h>

// Board bring-up only, before Host activation. Combine up to four providers
// under the existing peripherals extension. Providers assign distinct port IDs
// and permission scopes; their context and callbacks have board lifetime.
bool pxa_board_add_peripheral_backend(const pxa_peripherals_backend_t* backend);
