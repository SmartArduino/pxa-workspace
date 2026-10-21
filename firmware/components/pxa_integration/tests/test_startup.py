#!/usr/bin/env python3
"""Run the production startup and font-loader bodies against instrumented ports.

Only hardware/service boundaries are mocked. Extracting the two function bodies
lets this host test exercise actual initialization order without ESP-IDF/RTOS.
Use --integration-source to demonstrate the pre-fix regression.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile


def function(source: str, signature: str) -> str:
    begin = source.index(signature)
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m[0]), source, flags=re.S)
    start = masked.index('{', begin)
    depth = 1
    end = start + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[begin:end]


FIXTURE = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#define CONFIG_LV_USE_FREETYPE 1
#define CONFIG_PXADB_AUTOSTART 0
#define PXA_ESP_HOST_CJK_FONT_PATH "/font.ttf"
#define PXA_ESP_HOST_TAG "host-test"
#define LV_FREETYPE_FONT_RENDER_MODE_BITMAP 0
#define LV_FREETYPE_FONT_STYLE_NORMAL 0
const char* pxa_esp_system_font_path() { return PXA_ESP_HOST_CJK_FONT_PATH; }
const char* kTag = "startup-test";
uint32_t esp_log_timestamp() { return 0; }
template <typename... Args> void IgnoreLog(Args&&...) {}
#define ESP_LOGI(...) IgnoreLog(__VA_ARGS__)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
struct lv_font_t { const lv_font_t* fallback; unsigned line_height; };
struct lv_display_t { unsigned dpi = 130; } panel;
unsigned requested_dpi, last_pixels, host_calls, shell_calls, profile_reads;
unsigned initialize_calls, mounted_calls, lock_depth;
bool display_available, host_success, initialized, mounted;
bool clock_ready, clock_success=true;
bool pxa_system_clock_initialize() { assert(initialized); clock_ready=clock_success; return clock_success; }
std::vector<unsigned> font_pixels;
lv_display_t* lv_display_get_default() { return &panel; }
unsigned lv_display_get_dpi(lv_display_t* d) { assert(lock_depth); return d->dpi; }
void lv_display_set_dpi(lv_display_t* d, unsigned dpi) { assert(lock_depth); d->dpi=dpi; }
void lv_lock() { ++lock_depth; }
void lv_unlock() { assert(lock_depth); --lock_depth; }
lv_font_t* lv_freetype_font_create(const char*, int, unsigned size, int) {
    static lv_font_t font{};
    last_pixels=size; font_pixels.push_back(size); font.line_height=size+3; return &font;
}
// FONT_LOADER
struct pxsys_display_profile_t { unsigned width, height, density_dpi; };
struct pxa_product_profile_t { std::size_t struct_size; bool mount_storage; };
struct pxa_board_port_t {
    void* context;
    bool initialize_before_storage;
    bool (*initialize)(void*);
    lv_display_t* (*display)(void*);
    void (*display_profile)(void*, pxsys_display_profile_t*);
    bool (*set_level)(void*, int, unsigned);
    bool (*lock_display)(void*, unsigned);
    void (*unlock_display)(void*);
    void (*show_initial_frame)(void*);
    bool (*configure_diagnostics)(void*);
} board;
void* g_system;
struct { bool has_volume, has_brightness; unsigned volume, brightness; } g_preferences{};
constexpr int PXSYS_LEVEL_CONTROL_VOLUME=0, PXSYS_LEVEL_CONTROL_BRIGHTNESS=1;
const pxa_board_port_t* pxa_board_current() { return &board; }
bool initialize(void*) { ++initialize_calls; initialized=true; return true; }
bool MountPxaStorage() { ++mounted_calls; mounted=true; return true; }
lv_display_t* display(void*) { assert(initialized); return display_available?&panel:nullptr; }
void profile(void*, pxsys_display_profile_t* out) {
    assert(initialized); ++profile_reads; *out={480,480,requested_dpi};
}
bool pxa_host_initialize() {
    ++host_calls; assert(initialized && clock_ready);
    if(!host_success)return false;
    for(unsigned sp : {12u,14u,16u,20u,24u,28u}) {
        assert(load_ui_font(sp,nullptr));
        unsigned expected=(sp*(requested_dpi?requested_dpi:160)+80)/160;
        if(last_pixels!=expected)
            std::fprintf(stderr,"DPI=%u, %u sp: got %u px, expected %u px\n",requested_dpi,sp,last_pixels,expected);
        assert(last_pixels==expected); // fails if fonts saw LVGL's default 130 DPI
    }
    return true;
}
void ShowStartupRecovery(const pxa_board_port_t*) {}
void LoadUserPreferences() {}
bool lvgl_port_lock(unsigned) { return true; }
void lvgl_port_unlock() {}
bool CreateSystem(const pxa_board_port_t*,const pxa_product_profile_t*,
                  const pxsys_display_profile_t& input) {
    ++shell_calls; assert(host_calls==1 && profile_reads==1);
    assert(panel.dpi==(requested_dpi?requested_dpi:160));
    assert(input.density_dpi==panel.dpi && input.width==480 && input.height==480);
    g_system=&panel; return true;
}
// The baseline signature is accepted so the same test can prove old order fails.
bool CreateSystem(const pxa_board_port_t*,const pxa_product_profile_t*) {
    ++shell_calls; return true;
}
namespace pxadb { void Stop() {} }
bool pxa_host_start_runtime() { assert(shell_calls==1); return true; }
extern "C" void pxa_integration_stop() { g_system=nullptr; }
// STARTUP
int main() {
    for(unsigned dpi : {305u,0u,80u,130u,160u,216u,480u}) {
        for(bool early : {false,true}) {
            requested_dpi=dpi; panel.dpi=130; g_system=nullptr;
            host_calls=shell_calls=profile_reads=initialize_calls=mounted_calls=lock_depth=0;
            display_available=host_success=true; initialized=mounted=clock_ready=false; font_pixels.clear();
            board={nullptr,early,initialize,display,profile,nullptr,nullptr,nullptr,nullptr,nullptr};
            pxa_product_profile_t product{sizeof(pxa_product_profile_t),true};
            assert(pxa_integration_start(&product));
            assert(initialize_calls==1 && mounted_calls==1 && font_pixels.size()==6 && lock_depth==0);
        }
    }
    g_system=nullptr; display_available=false; host_calls=shell_calls=0;
    pxa_product_profile_t product{sizeof(pxa_product_profile_t),true};
    assert(!pxa_integration_start(&product) && host_calls==0 && shell_calls==0);
    display_available=true; host_success=false; host_calls=shell_calls=0;
    assert(!pxa_integration_start(&product) && host_calls==1 && shell_calls==0);
    host_success=true; clock_success=false; host_calls=shell_calls=0;
    assert(!pxa_integration_start(&product) && host_calls==0 && shell_calls==0);
    clock_success=true; initialized=mounted=clock_ready=false;
    host_calls=shell_calls=profile_reads=initialize_calls=mounted_calls=0;
    product.mount_storage=false;
    assert(pxa_integration_start(&product));
    assert(clock_ready && !mounted && mounted_calls==0 && initialize_calls==1);
    assert(lock_depth==0);
    std::puts("Production startup/font loader: density before fonts, both mount orders, clock before Host, optional storage and failures passed");
}
'''


def main():
    directory = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--integration-source', type=Path, default=directory.parent/'pxa_integration.cc')
    args = parser.parse_args()
    host = directory.parents[1]/'pxa/src/runtime/pxa_esp_host.c'
    source = FIXTURE.replace('// FONT_LOADER', function(host.read_text(), 'static lv_font_t *load_ui_font('))
    source = source.replace('// STARTUP', function(args.integration_source.read_text(), 'extern "C" bool pxa_integration_start('))
    with tempfile.TemporaryDirectory(prefix='pxa-startup-') as temp:
        unit = Path(temp)/'startup.cpp'
        unit.write_text(source)
        output = Path(temp)/'startup'
        subprocess.run([os.environ.get('CXX','clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(unit), '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True)


if __name__ == '__main__':
    main()
