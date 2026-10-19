#!/usr/bin/env python3
"""Run production volume setters and NVS preference bodies across a reboot."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
from test_startup import function

FIXTURE=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
constexpr int ESP_OK=0,NVS_READONLY=0,NVS_READWRITE=1;
constexpr int PXSYS_COLOR_SCHEME_DARK=1,PXSYS_THEME_PALETTE_COUNT=8;
using nvs_handle_t=int;using esp_err_t=int;
struct pxsys_reference_idle_policy_t {uint16_t dim_after_seconds,lock_after_seconds;uint8_t dim_percent;};
struct pxsys_system_status_snapshot_t {bool volume_supported;uint8_t volume_percent;bool brightness_supported;uint8_t brightness_percent;};
std::map<std::string,uint8_t> flash;
unsigned commits=0;
int nvs_open(const char*,int mode,int* h) {*h=1;return mode==NVS_READONLY && flash.empty()?-1:0;}
void nvs_close(int) {}
int nvs_get_u8(int,const char* key,uint8_t* v) {auto i=flash.find(key);if(i==flash.end())return -1;*v=i->second;return 0;}
int nvs_get_u16(int,const char*,uint16_t*) {return -1;}
int nvs_set_u8(int,const char* key,uint8_t v) {flash[key]=v;return 0;}
int nvs_commit(int) {++commits;return 0;}
// PREFERENCES
const char *kPreferencesNamespace="pxa_user";
UserPreferences g_preferences;
bool g_status_observer_seen=false;
uint8_t g_observed_volume=0,g_observed_brightness=0;
// LOAD
// SAVE
struct Audio {
    uint8_t value=65;
    void SetVolume(uint8_t v) {value=v>100?100:v;}
    uint8_t volume() const {return value;}
};
class Gamekit {
public:
    Audio audio_;
    bool attached=false;
    void SetVolume(uint8_t value);
    void PublishStatus() {
        if(!attached)return;
        pxsys_system_status_snapshot_t status{true,audio_.volume(),true,60};
        SaveLevels(nullptr,&status);
    }
    void Attach() {attached=true;PublishStatus();}
};
// SETTER
int main() {
    LoadUserPreferences();assert(!g_preferences.has_volume);
    Gamekit first;first.Attach();assert(commits==0);
    first.SetVolume(35);
    // No status timer is ticked between the volume key and the restart.
    assert(commits==1 && flash.at("volume")==35);
    first.SetVolume(35);assert(commits==1);
    auto restart=[]() {
        g_status_observer_seen=false;g_observed_volume=g_observed_brightness=0;
        LoadUserPreferences();assert(g_preferences.has_volume);
        Gamekit board;board.SetVolume(g_preferences.volume);board.Attach();return board;
    };
    Gamekit second=restart();assert(second.audio_.volume()==35 && commits==1);
    second.SetVolume(0);assert(flash.at("volume")==0);
    Gamekit third=restart();assert(third.audio_.volume()==0);
    third.SetVolume(100);Gamekit fourth=restart();assert(fourth.audio_.volume()==100);
    auto count=commits;fourth.PublishStatus();assert(commits==count);
    puts("Volume preferences: immediate restart, restored 35/0/100, duplicate write suppression passed");
}
'''

def main():
    directory=Path(__file__).resolve().parent
    root=directory.parents[3]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--board-source',type=Path,default=root/'firmware/boards/doit-gamekit/src/doit_gamekit_board.cc')
    a=p.parse_args()
    integration=(directory.parent/'pxa_integration.cc').read_text()
    board=a.board_source.read_text()
    setter=(function(board,'void Gamekit::SetVolume(') if 'void Gamekit::SetVolume(' in board
            else function(board,'void SetVolume(').replace('void SetVolume(','void Gamekit::SetVolume(',1))
    unit=FIXTURE.replace('// PREFERENCES',function(integration,'struct UserPreferences')+';')
    unit=unit.replace('// LOAD',function(integration,'void LoadUserPreferences('))
    unit=unit.replace('// SAVE',function(integration,'void SaveLevels(')).replace('// SETTER',setter)
    with tempfile.TemporaryDirectory(prefix='pxa-volume-') as temp:
        source=Path(temp)/'test.cpp';source.write_text(unit);binary=Path(temp)/'test'
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__':main()
