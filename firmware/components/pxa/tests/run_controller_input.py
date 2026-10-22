#!/usr/bin/env python3
"""Exercise the real owner-task controller subscription cache and board query."""
from pathlib import Path
import argparse
import subprocess

SOURCE = Path(__file__).resolve().parents[1] / 'src/runtime/pxa_esp_host.c'

def function(source, name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

SHIM = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define PXA_COMPONENT_INVALID 0
#define PXA_UI_PRIMARY_SURFACE 1
#define PXA_UI_EVENT_KEY 3
#define PXA_UI_EVENT_CONTROLLER_STATE 10
static int g_public_state_lock, g_process_state_lock, locked;
#define portENTER_CRITICAL(p) do {(void)(p);assert(!locked);locked=1;} while(0)
#define portEXIT_CRITICAL(p) do {(void)(p);assert(locked);locked=0;} while(0)
static uint64_t root_mask;
static unsigned registry_reads;
static struct {
    unsigned initialized, ui_foreground_requested;
    struct {struct {void *ui;} services;unsigned ui_component,active_component;} activation;
    struct {uint8_t main_active,volume_key_capture_active,controller_input_active;} public_state;
} g_host;
static bool pxa_ui_accepts_event(void *ui,unsigned component,unsigned surface,unsigned node,unsigned kind) {
    assert(!locked && ui && component==7 && surface==1 && node==1);
    ++registry_reads;return !!(root_mask & (UINT64_C(1) << kind));
}
'''
CASES = r'''
int main(void) {
    assert(!pxa_esp_host_accepts_controller_input());
    g_host.initialized=1;g_host.ui_foreground_requested=1;
    g_host.public_state.main_active=1;
    g_host.activation.services.ui=&g_host;
    g_host.activation.ui_component=g_host.activation.active_component=7;
    // Canvas and Surface share the root subscription. No rendering heuristic.
    root_mask=UINT64_C(1)<<PXA_UI_EVENT_CONTROLLER_STATE;
    refresh_input_capture_state();assert(pxa_esp_host_accepts_controller_input());
    assert(!g_host.public_state.volume_key_capture_active);
    unsigned reads=registry_reads;
    for(unsigned n=0;n<10000;++n)assert(pxa_esp_host_accepts_controller_input());
    assert(registry_reads==reads); // Polling must never traverse another task's UI.
    root_mask|=UINT64_C(1)<<PXA_UI_EVENT_KEY;
    refresh_input_capture_state();assert(g_host.public_state.volume_key_capture_active);
    g_host.ui_foreground_requested=0;assert(!pxa_esp_host_accepts_controller_input());
    g_host.ui_foreground_requested=1;assert(pxa_esp_host_accepts_controller_input());
    root_mask=UINT64_C(1)<<PXA_UI_EVENT_KEY;
    refresh_input_capture_state();assert(!pxa_esp_host_accepts_controller_input());
    assert(g_host.public_state.volume_key_capture_active);
    root_mask=UINT64_C(1)<<PXA_UI_EVENT_CONTROLLER_STATE;
    refresh_input_capture_state();assert(pxa_esp_host_accepts_controller_input());
    g_host.public_state.main_active=0;assert(!pxa_esp_host_accepts_controller_input());
    g_host.public_state.main_active=1;g_host.activation.services.ui=NULL;
    refresh_input_capture_state();assert(!pxa_esp_host_accepts_controller_input());
    assert(!g_host.public_state.volume_key_capture_active);
    return 0;
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True, help='Ignored local artifacts directory')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text()
    test = args.output / 'controller_input.c'
    binary = args.output / 'controller_input'
    test.write_text(SHIM + function(source, 'refresh_input_capture_state') + '\n' +
                    function(source, 'pxa_esp_host_accepts_controller_input') + CASES)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-g',str(test),'-o',str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('PASS: owner cache, Canvas subscription, foreground, stop, unsubscribe and lock discipline')

if __name__ == '__main__':
    main()
