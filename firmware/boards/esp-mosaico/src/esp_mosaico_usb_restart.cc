// SPDX-License-Identifier: MIT
#include "esp_mosaico_usb_restart.h"
#include "sdkconfig.h"
#include <atomic>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_private/system_internal.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/lp_system_reg.h>
#include <soc/soc.h>

namespace {
DRAM_ATTR std::atomic<bool> download_reset{false};
}

extern "C" void __real_esp_cpu_reset(int core_id);

extern "C" void IRAM_ATTR __wrap_esp_cpu_reset(int core_id) {
    if (download_reset.load(std::memory_order_relaxed) && core_id == esp_cpu_get_core_id()) {
        // esp_restart_noos has now stopped the other CPU and DMA, written back
        // PSRAM and restored CPU/MEM/APB clocks. Its ordinary final CPU reset
        // leaves USB state intact on S31. Use the ROM digital system reset here.
        REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
        esp_rom_software_reset_system();
        while (true) {}
    }
    __real_esp_cpu_reset(core_id);
}

[[noreturn]] void EspMosaicoRestartToUsbDownload() {
    download_reset.store(true, std::memory_order_relaxed);
    // Match esp_restart's scheduler boundary. The caller sent SHUTDOWN before
    // dismantling the USB console, so handlers must not be notified again.
#if CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
    vTaskPreemptionDisable(nullptr);
#else
    vTaskSuspendAll();
#endif
    esp_restart_noos();
}
