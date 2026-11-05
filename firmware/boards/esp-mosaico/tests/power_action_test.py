#!/usr/bin/env python3
"""Exercise the production power actions with host-side board stubs."""
import os
from pathlib import Path
import subprocess
import sys


tests_dir = Path(__file__).resolve().parent
build_dir = Path(sys.argv[1])
source = (tests_dir / "../src/esp_mosaico_hardware.cc").read_text()
start = source.index("bool EspMosaicoHardware::RequestPowerAction(")
end = source.index("void EspMosaicoHardware::PollBattery(", start)
program = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>

constexpr int PXSYS_REFERENCE_POWER_ACTION_RESTART = 0;
constexpr int PXSYS_REFERENCE_POWER_ACTION_SHUTDOWN = 1;
constexpr int ESP_OK = 0;
constexpr int GPIO_MODE_OUTPUT_OD = 1;
constexpr int LP_SYSTEM_REG_SYS_CTRL_REG = 0;
constexpr int LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT = 4;
using gpio_num_t = int;
namespace mosaico_board {
constexpr int kPeripheralPower = 60, kAudioAmplifierEnable = 45, kPowerSwitch = 57, kDisplayData0 = 36;
}
bool save_ok = true, cleanup_ok = true, gpio_ok = true, display_ok = true;
bool download_flag = false;
std::vector<int> stages;
void pxa_esp_surface_set_display_unlocked(bool) {}
bool pxa_host_set_display_interactive(bool) { return true; }
bool pxa_host_prepare_shutdown(int) { stages.push_back(1); return save_ok; }
bool pxa_system_clock_save() { stages.push_back(2); return true; }
bool EspMosaicoStopPeripherals() { stages.push_back(3); return cleanup_ok; }
int ConfigureOutput(int pin, int level, int = 0) {
    assert((pin == 60 || pin == 36) && level == 1);
    stages.push_back(pin == 60 ? 4 : 8);
    return gpio_ok ? ESP_OK : -1;
}
void vTaskDelay(int) {}
int pdMS_TO_TICKS(int value) { return value; }
void REG_SET_BIT(int, int bit) {
    assert(bit == LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    download_flag = true;
    stages.push_back(5);
}
[[noreturn]] void esp_restart() { stages.push_back(6); throw 6; }
constexpr int ESP_SYS_EVENT_SHUTDOWN = 1;
void esp_sys_event_trigger(int event, void* context) {
    assert(event == ESP_SYS_EVENT_SHUTDOWN && context == nullptr);
    stages.push_back(9);
}
namespace pxadb { void Stop() { stages.push_back(10); } }
int tinyusb_driver_uninstall() { stages.push_back(11); return ESP_OK; }
void usb_utmi_ll_enable_bus_clock(bool enabled) { assert(enabled); stages.push_back(12); }
void usb_utmi_ll_reset_register() { stages.push_back(13); }
[[noreturn]] void EspMosaicoRestartToUsbDownload() {
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
bool EspMosaicoVibrate(int) { return true; }
int esp_lv_adapter_lock(int) { return ESP_OK; }
void esp_lv_adapter_unlock() {}
void lv_lock() {}
void lv_unlock() {}
int gpio_set_level(int, int) { return ESP_OK; }
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
class EspMosaicoHardware {
public:
    static constexpr int8_t kUsbDownloadAction = 2;
    void* touch_task_ = this;
    std::atomic<int8_t> power_action_{-1};
    bool power_menu_pending_ = false, wake_pending_ = false;
    struct Audio { void StopForShutdown() {} } audio_;
    void IdleScreenOff() {}
    bool RequestPowerAction(int8_t action);
    bool QuiesceDisplayForRestart() { stages.push_back(7); return display_ok; }
    void PerformPowerAction();
};
'''
program += source[start:end]
program += r'''
int main() {
    EspMosaicoHardware board;
    board.touch_task_ = nullptr;
    assert(!board.RequestPowerAction(board.kUsbDownloadAction));
    board.touch_task_ = &board;
    assert(!board.RequestPowerAction(99));
    assert(board.RequestPowerAction(board.kUsbDownloadAction));
    assert(board.RequestPowerAction(board.kUsbDownloadAction));
    assert(!board.RequestPowerAction(PXSYS_REFERENCE_POWER_ACTION_RESTART));
    try { board.PerformPowerAction(); assert(false); } catch (int value) { assert(value == 6); }
    assert(download_flag);
    assert((stages == std::vector<int>{1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13, 5, 6}));

    for (int failure = 0; failure < 4; ++failure) {
        EspMosaicoHardware failed;
        stages.clear(); download_flag = false;
        save_ok = failure != 0; cleanup_ok = failure != 1; gpio_ok = failure != 2;
        display_ok = failure != 3;
        assert(failed.RequestPowerAction(failed.kUsbDownloadAction));
        failed.PerformPowerAction();
        assert(!download_flag && failed.power_action_ == -1);
        assert(failed.power_menu_pending_ && failed.wake_pending_);
        assert(failed.RequestPowerAction(failed.kUsbDownloadAction));
    }

    save_ok = cleanup_ok = gpio_ok = display_ok = true;
    EspMosaicoHardware normal;
    stages.clear(); download_flag = false;
    assert(normal.RequestPowerAction(PXSYS_REFERENCE_POWER_ACTION_RESTART));
    assert(!normal.RequestPowerAction(normal.kUsbDownloadAction));
    try { normal.PerformPowerAction(); assert(false); } catch (int value) { assert(value == 6); }
    assert(!download_flag);
    assert((stages == std::vector<int>{1, 2, 3, 6}));
}
'''
build_dir.mkdir(parents=True, exist_ok=True)
test_source = build_dir / "power_action_test.cc"
test_binary = build_dir / "power_action_test"
test_source.write_text(program)
subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                str(test_source), "-o", str(test_binary)], check=True)
subprocess.run([str(test_binary)], check=True)
print("Mosaico USB download: save/cleanup/strap ordering, failure retry and power action exclusion passed")
