# SPDX-License-Identifier: MIT
"""Exercise the production reset hook, including ordinary CPU resets."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#define DRAM_ATTR
#define IRAM_ATTR
constexpr uint32_t LP_SYSTEM_REG_SYS_CTRL_REG = 0x20700008;
constexpr uint32_t LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT = 4;
std::vector<std::string> events;
int current_core;
struct Restart {};
int esp_cpu_get_core_id() { return current_core; }
extern "C" void __wrap_esp_cpu_reset(int);
extern "C" void __real_esp_cpu_reset(int core) { events.push_back("cpu:" + std::to_string(core)); }
void SetBit(uint32_t address, uint32_t value) {
    assert(address == 0x20700008 && value == 4); events.push_back("force-rom");
}
#define REG_SET_BIT(address, value) SetBit(address, value)
void esp_rom_software_reset_system() { events.push_back("system-reset"); throw Restart{}; }
void vTaskPreemptionDisable(void* task) { assert(!task); events.push_back("scheduler"); }
void vTaskSuspendAll() { events.push_back("scheduler"); }
[[noreturn]] void esp_restart_noos() {
    events.push_back("idf-cleanup");
    __wrap_esp_cpu_reset(1 - current_core);
    __wrap_esp_cpu_reset(current_core);
    assert(false);
    throw Restart{};
}
// PRODUCTION
int main() {
    for (int core = 0; core < 2; ++core) {
        current_core = core;
        __wrap_esp_cpu_reset(0);
        __wrap_esp_cpu_reset(1);
    }
    assert((events == std::vector<std::string>{"cpu:0", "cpu:1", "cpu:0", "cpu:1"}));
    for (int core = 0; core < 2; ++core) {
        current_core = core;
        events.clear();
        try { EspMosaicoRestartToUsbDownload(); } catch (const Restart&) {}
        assert((events == std::vector<std::string>{"scheduler", "idf-cleanup",
            "cpu:" + std::to_string(1 - core), "force-rom", "system-reset"}));
        download_reset = false;
    }
}
'''


class UsbRestartTests(unittest.TestCase):
    def test_reset_scope_and_scheduler_variants(self):
        source = (ROOT / "src/esp_mosaico_usb_restart.cc").read_text()
        source = re.sub(r"^#include[^\n]*", "", source, flags=re.M)
        with tempfile.TemporaryDirectory(prefix="mosaico-reset-") as temporary:
            path = Path(temporary)
            unit = path / "test.cc"
            unit.write_text(FIXTURE.replace("// PRODUCTION", source))
            for smp in (0, 1):
                binary = path / f"test-{smp}"
                subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                                "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                f"-DCONFIG_FREERTOS_SMP={smp}", "-DCONFIG_FREERTOS_UNICORE=0",
                                str(unit), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
