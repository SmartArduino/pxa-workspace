#!/usr/bin/env python3
"""Exercise the production system-owned move job without ESP task hardware."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT/'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "pxa_esp_install_approval.h"
#include "pxa_esp_ui_shell.h"
#include "pxa_esp_install_capacity.h"
#include "pxa/pxa_host.h"
#define EXT_RAM_BSS_ATTR
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
struct { uint32_t next_permission_prompt_id;struct {char active_identity[130];} activation;
    bool graceful_stop_active,cooperative_active;char deferred_launch_identity[130];char locale[64]; } g_host{};
void* g_store_install;
void* g_native_file;
struct {uint32_t prompt_id;} g_store_result{},g_store_uninstall{};
bool ime_active,post_ok=true,progress_ok=true;
bool legacy_upgrade;
unsigned prepared,committed,notified,toasts,dismissed,modal_depth;
int allocations_before_failure=-1;
pxa_status_t prepare_status=0,commit_status=0,last_result=999;
uint32_t last_result_id;
pxa_esp_ui_install_prompt_t posted{};
struct UiNotice {uint32_t id;bool busy;};
std::vector<UiNotice> ui_notices;
uint32_t visible_prompt;
bool visible_busy;
// LVGL inserts async timers at the head. Exercise reverse delivery so an
// older dismissal cannot erase the newly posted progress overlay.
void pump_ui() {
    while(!ui_notices.empty()) {
        const auto notice=ui_notices.back();ui_notices.pop_back();
        if(notice.busy) {visible_prompt=notice.id;visible_busy=true;}
        else if(visible_prompt==notice.id) {visible_prompt=0;visible_busy=false;}
    }
}
// Preserve C's implicit void* conversion at the mocked allocator boundary.
struct Allocation {void* value;template<class T> operator T*() const {return static_cast<T*>(value);}};
Allocation heap_caps_calloc(size_t n,size_t size,unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(allocations_before_failure==0)return {nullptr};
    if(allocations_before_failure>0)--allocations_before_failure;
    return {calloc(n,size)};
}
bool pxa_esp_host_locale_is_chinese() {return true;}
bool pxa_esp_ime_active() {return ime_active;}
void pxa_esp_surface_runtime_modal_enter() {assert(!modal_depth);++modal_depth;}
void pxa_esp_surface_runtime_modal_leave() {assert(modal_depth==1);--modal_depth;}
void pxa_esp_ui_shell_post_toast(const char* text,uint32_t duration) {assert(text && duration);++toasts;}
int pxa_esp_ui_shell_post_install_prompt(const pxa_esp_ui_install_prompt_t* prompt) {
    pump_ui();posted=*prompt;if(post_ok) {visible_prompt=prompt->prompt_id;visible_busy=false;}return post_ok;
}
void pxa_esp_ui_shell_dismiss_install_prompt(uint32_t id) {assert(id);++dismissed;ui_notices.push_back({id,false});}
int pxa_esp_ui_shell_post_move_progress_kind(uint32_t id,const char* name,uint8_t) {
    assert(id && name[0] && modal_depth==1);if(progress_ok)ui_notices.push_back({id,true});return progress_ok;
}
void pxa_esp_ui_shell_post_move_result(uint32_t id,int32_t status) {
    assert(!modal_depth);last_result_id=id;last_result=status;ui_notices.push_back({id,false});
}
void pxa_host_notify_catalog_changed() {assert(!modal_depth);++notified;}
pxa_status_t pxa_esp_package_store_prepare_move(const char* identity,pxa_esp_move_approval_t* approval) {
    ++prepared;assert(!g_host.activation.active_identity[0] && !strcmp(identity,"publisher:game"));
    if(prepare_status)return prepare_status;
    strcpy(approval->identity,identity);approval->source.generation=7;approval->source.install_id[0]=42;
    strcpy(approval->name,"Fallback name");strcpy(approval->current_volume,"SD");strcpy(approval->media_volume,"SD");
    approval->count=legacy_upgrade?1:2;approval->legacy_upgrade=legacy_upgrade;
    for(unsigned i=0;i<2;++i) {
        strcpy(approval->choices[i].volume.name,i?"Other flash":"Internal");
        approval->choices[i].volume.mount_epoch=11+i;approval->choices[i].volume.free_bytes=1000;
        approval->choices[i].estimate.volumes[0].bytes=500+i;
    }
    return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_prepare_move_resources(const char* identity,pxa_esp_move_approval_t* approval) {
    pxa_status_t status=pxa_esp_package_store_prepare_move(identity,approval);
    if(!status) {
        approval->resources_only=1;approval->source_media_epoch=99;
        approval->choices[1].estimate.volume_count=2;
        approval->choices[1].estimate.volumes[0].mount_epoch=42;
        approval->choices[1].estimate.volumes[1].mount_epoch=12;
        approval->choices[1].estimate.volumes[1].bytes=777;
    }
    return status;
}
bool pxa_esp_host_resolve_package_metadata(const char* identity,const char* locale,pxa_host_package_metadata_t* metadata) {
    assert(!strcmp(identity,"publisher:game") && !strcmp(locale,"zh-CN"));strcpy(metadata->name,"雷神之锤");return true;
}
pxa_status_t pxa_esp_package_store_commit_move(const pxa_esp_move_approval_t* approval,size_t choice,
    pxa_esp_transfer_progress_fn,void*) {
    pump_ui();assert(visible_prompt==posted.prompt_id && visible_busy);
    assert(modal_depth==1 && approval->source.generation==7 && approval->source.install_id[0]==42);
    assert(choice==1 && approval->choices[choice].volume.mount_epoch==12);++committed;return commit_status;
}
// PRODUCTION
int main() {
    strcpy(g_host.locale,"zh-CN");
    g_host.cooperative_active=true;native_move_prepare("publisher:game");
    assert(!prepared && !g_native_move && !toasts);g_host.cooperative_active=false;
    strcpy(g_host.activation.active_identity,"running");native_move_prepare("publisher:game");
    assert(!prepared && !g_native_move);g_host.activation={};
    native_move_prepare("publisher:game");
    assert(g_native_move && modal_depth==1 && posted.move && posted.count==2 && posted.volumes[1].required_bytes==501);
    assert(!strcmp(posted.app_name,"雷神之锤") && !strcmp(posted.retained_media_volume,"SD"));
    uint32_t first=posted.prompt_id;native_move_decide(false,0);
    assert(!g_native_move && !modal_depth && !committed);
    g_host.activation={}; // Guest activation teardown cannot recycle system prompt IDs.
    native_move_prepare("publisher:game");assert(posted.prompt_id>first);
    uint32_t second=posted.prompt_id;native_move_decide(true,2);
    assert(!g_native_move && !modal_depth && committed==1 && notified==1 && last_result==0 && last_result_id==second);
    native_move_prepare("publisher:game");native_move_decide(true,64);
    assert(!g_native_move && !modal_depth && committed==1);
    native_move_prepare("publisher:game");strcpy(g_host.activation.active_identity,"new-running");native_move_decide(true,2);
    assert(!g_native_move && !modal_depth && committed==1);g_host.activation={};
    post_ok=false;native_move_prepare("publisher:game");assert(!g_native_move && !modal_depth);post_ok=true;
    progress_ok=false;native_move_prepare("publisher:game");native_move_decide(true,2);
    assert(!g_native_move && !modal_depth && committed==1 && last_result==PXA_STATUS_RESOURCE_LIMIT);progress_ok=true;
    prepare_status=PXA_STATUS_UNSUPPORTED;native_move_prepare("publisher:game");assert(!g_native_move && !modal_depth);prepare_status=0;
    allocations_before_failure=0;native_move_prepare("publisher:game");assert(!g_native_move && !modal_depth);
    allocations_before_failure=1;native_move_prepare("publisher:game");assert(!g_native_move && !modal_depth);allocations_before_failure=-1;
    native_move_prepare("publisher:game");native_move_cancel();assert(!g_native_move && !modal_depth);
    commit_status=PXA_STATUS_BAD_STATE;native_move_prepare("publisher:game");native_move_decide(true,2);
    assert(!g_native_move && !modal_depth && last_result==PXA_STATUS_BAD_STATE && committed==2);
    pump_ui();assert(!visible_prompt && !visible_busy);
    commit_status=0;native_resources_prepare("publisher:game");
    assert(g_native_move && posted.resources_only && posted.move && posted.volumes[1].required_bytes==777);
    native_move_decide(false,0);assert(!g_native_move && !modal_depth && committed==2);
    native_resources_prepare("publisher:game");native_move_decide(true,2);
    assert(!g_native_move && !modal_depth && committed==3 && last_result==0);
    pump_ui();assert(!visible_prompt && !visible_busy);
    legacy_upgrade=true;native_move_prepare("publisher:game");
    assert(g_native_move && posted.legacy_upgrade && posted.move && posted.count==1);
    assert(!posted.resources_only && !strcmp(posted.retained_media_volume,"SD"));
    native_move_decide(false,0);pump_ui();
    assert(!g_native_move && !modal_depth && committed==3 && !visible_prompt);
    puts("Native move UI: system ownership, monotonic prompts, localization, cancel/invalid/stale-active decisions, reverse async delivery, UI allocation/post failure and balanced modal lifetime passed");
}
'''


def main():
    host = (ROOT/'firmware/components/pxa/src/runtime/pxa_esp_host.c').read_text()
    start = host.index('typedef struct {\n    pxa_esp_move_approval_t approval;')
    end = host.index('static void native_move_cancel(', start)
    production = host[start:end] + '\n'.join(function(host, signature) for signature in (
        'static void copy_utf8_c_string(char *output, size_t capacity,\n                               const char *value) {', 'static void native_move_cancel(',
        'static __attribute__((noinline)) void native_move_prepare_kind(',
        'static void native_move_prepare(', 'static void native_resources_prepare(', 'static void native_move_decide('))
    source = FIXTURE.replace('// PRODUCTION', production)
    with tempfile.TemporaryDirectory(prefix='pxa-native-move-') as directory:
        unit=Path(directory)/'move.cpp';binary=Path(directory)/'move'
        unit.write_text(source)
        includes=['firmware/components/pxa/src/package','firmware/components/pxa/src/services','firmware/components/pxa/src/ui',
                  'firmware/components/pxa/include','deps/pxa-system/libpxa/include','deps/pxa-system/libpxa/adapters/include']
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            '-DESP_PLATFORM=1','-DCONFIG_PXA_ENABLED=1',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',*[f'-I{ROOT/path}' for path in includes],
            str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
