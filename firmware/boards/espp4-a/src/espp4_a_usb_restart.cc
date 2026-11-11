// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_usb.h"
#include <atomic>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <soc/lp_system_reg.h>
#include <soc/soc.h>

namespace {
DRAM_ATTR std::atomic<bool> download_reset{false};
}

extern "C" void __real_esp_cpu_reset(int core_id);
extern "C" void IRAM_ATTR __wrap_esp_cpu_reset(int core_id) {
    if (download_reset.load(std::memory_order_relaxed) && core_id == esp_cpu_get_core_id()) {
        // IDF has stopped the other core/DMA, flushed PSRAM and restored clocks.
        // Its normal CPU-only reset retains OTG state; ROM needs a system reset.
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
        esp_rom_software_reset_system();
        while (true) {}
    }
    __real_esp_cpu_reset(core_id);
}

[[noreturn]] void Espp4ARestartToUsbDownload() {
    download_reset.store(true, std::memory_order_relaxed);
    esp_restart();
    while (true) {}
}
