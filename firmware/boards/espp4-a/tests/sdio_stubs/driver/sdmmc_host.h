// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sdmmc_cmd.h>
constexpr int SDMMC_HOST_SLOT_0 = 0;
esp_err_t sdmmc_host_do_transaction(int, sdmmc_command_t*);
