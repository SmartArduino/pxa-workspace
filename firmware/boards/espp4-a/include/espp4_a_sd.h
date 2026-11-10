// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sdmmc_cmd.h>

// Keep the shared controller assigned to the TF card until programming ends.
esp_err_t espp4_a_sd_transaction(int slot, sdmmc_command_t* command);
bool espp4_a_sd_device_test(const char* mount);
