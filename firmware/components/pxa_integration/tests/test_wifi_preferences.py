#!/usr/bin/env python3
"""Exercise production Wi-Fi preferences and GameKit 1 radio control across boots."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
from test_startup import function


FIXTURE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
constexpr int ESP_OK=0, NVS_READONLY=0, NVS_READWRITE=1;
using nvs_handle_t=int;
using esp_err_t=int;
bool stored=false, fail_open=false, fail_write=false, fail_commit=false;
uint8_t saved=0, pending=0;
bool has_pending=false;
unsigned writes=0, commits=0, opens=0, closes=0;
int nvs_open(const char* name, int, int* handle) {
    assert(std::strcmp(name,"pxa_user")==0);
    if(fail_open)return -1;
    *handle=1; ++opens; has_pending=false; return ESP_OK;
}
int nvs_get_u8(int, const char* key, uint8_t* value) {
    assert(std::strcmp(key,"wifi")==0);
    if(!stored)return -1;
    *value=saved; return ESP_OK;
}
int nvs_set_u8(int, const char* key, uint8_t value) {
    assert(std::strcmp(key,"wifi")==0);
    ++writes;
    if(fail_write)return -1;
    pending=value; has_pending=true; return ESP_OK;
}
int nvs_commit(int) {
    ++commits;
    if(fail_commit)return -1;
    assert(has_pending); saved=pending; stored=true; return ESP_OK;
}
void nvs_close(int) {++closes; has_pending=false;}
// PREFERENCES
enum class WifiEvent {Connected};
struct WifiManagerConfig {std::string ssid_prefix, language;};
struct WifiManager {
    unsigned starts=0, stops=0, ap_starts=0, ap_stops=0;
    bool initialize_ok=true;
    static WifiManager& GetInstance() {static WifiManager manager; return manager;}
    template<class F> void SetEventCallback(F) {}
    bool Initialize(const WifiManagerConfig&) {return initialize_ok;}
    void StartStation() {assert(pxa_board_wifi_get_enabled()); ++starts;}
    void StopStation() {assert(!pxa_board_wifi_get_enabled()); ++stops;}
    void StartConfigAp() {assert(stored && saved==1); ++ap_starts;}
    void StopConfigAp() {++ap_stops;}
};
struct esp_sntp_config_t {bool start, wait_for_sync;};
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(...) esp_sntp_config_t{true,true}
int esp_netif_sntp_init(const esp_sntp_config_t*) {return ESP_OK;}
int esp_netif_sntp_start() {return ESP_OK;}
class Gamekit {
public:
    std::atomic<bool> wifi_enabled_{true};
    bool wifi_ready_=false, sntp_ready_=false;
    unsigned publications=0;
    bool published_enabled=false;
    bool InitializeWifi();
    bool SetWifi(bool enabled);
    void ProvisionWifi();
    void PublishStatus() {++publications; published_enabled=wifi_enabled_.load();}
};
// BOARD
void Boot(Gamekit& board) {board.wifi_ready_=board.InitializeWifi();}
int main() {
    auto& radio=WifiManager::GetInstance();
    assert(pxa_board_wifi_get_enabled());
    Gamekit first; Boot(first);
    assert(first.wifi_ready_ && first.wifi_enabled_ && radio.starts==1);
    assert(writes==0 && commits==0); // Boot never creates or overwrites preferences.
    assert(first.SetWifi(false));
    assert(stored && saved==0 && radio.stops==1 && !first.published_enabled);
    assert(commits==1);
    Gamekit disabled; Boot(disabled);
    assert(disabled.wifi_ready_ && !disabled.wifi_enabled_ && radio.starts==1);
    assert(disabled.SetWifi(false) && commits==1); // No duplicate Flash write.
    assert(disabled.SetWifi(true) && saved==1 && radio.starts==2);
    assert(disabled.published_enabled && commits==2);
    Gamekit enabled; Boot(enabled);
    assert(enabled.wifi_enabled_ && radio.starts==3 && commits==2);

    // A failed save must leave the current radio state and UI unchanged.
    for(int fault=0; fault<3; ++fault) {
        fail_open=fault==0; fail_write=fault==1; fail_commit=fault==2;
        const auto stops=radio.stops, publications=enabled.publications;
        assert(!enabled.SetWifi(false));
        assert(enabled.wifi_enabled_ && radio.stops==stops);
        assert(enabled.publications==publications);
    }
    fail_open=fail_write=fail_commit=false;
    assert(enabled.SetWifi(false));
    Gamekit provisioning; Boot(provisioning);
    assert(!provisioning.wifi_enabled_);
    const auto starts=radio.starts;
    provisioning.ProvisionWifi();
    assert(provisioning.wifi_enabled_ && saved==1 && radio.ap_starts==1);
    assert(provisioning.published_enabled && radio.starts==starts);
    Gamekit after_provisioning; Boot(after_provisioning);
    assert(after_provisioning.wifi_enabled_ && radio.starts==starts+1);

    // Invalid data/read failures retain the existing default-on behavior.
    saved=255;
    assert(pxa_board_wifi_get_enabled());
    fail_open=true;
    assert(pxa_board_wifi_get_enabled());
    fail_open=false;
    radio.initialize_ok=false;
    Gamekit unavailable; Boot(unavailable);
    assert(!unavailable.wifi_ready_ && !unavailable.SetWifi(false));
    const auto ap_starts=radio.ap_starts;
    unavailable.ProvisionWifi();
    assert(radio.ap_starts==ap_starts);
    assert(opens==closes);
    std::puts("Wi-Fi preferences: off/on boots, immediate persistence, provisioning, defaults and NVS failures passed");
}
'''


def main():
    directory = Path(__file__).resolve().parent
    root = directory.parents[3]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board-source', type=Path, default=
                        root/'firmware/boards/doit-gamekit-1/src/doit_gamekit_1_board.cc')
    args = parser.parse_args()
    api = (directory.parents[1]/'pxa_board_api/pxa_board_api.c').read_text()
    board = args.board_source.read_text()
    unit = FIXTURE.replace('// PREFERENCES', '\n'.join(
        function(api, signature) for signature in (
            'bool pxa_board_wifi_get_enabled(', 'bool pxa_board_wifi_set_enabled(')))
    unit = unit.replace('// BOARD', '\n'.join(
        function(board, signature) for signature in (
            'bool Gamekit::InitializeWifi(', 'bool Gamekit::SetWifi(',
            'void Gamekit::ProvisionWifi(')))
    with tempfile.TemporaryDirectory(prefix='pxa-wifi-') as temp:
        source = Path(temp)/'test.cpp'
        source.write_text(unit)
        binary = Path(temp)/'test'
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall',
                        '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
