#!/usr/bin/env python3
"""Exercise the production Files runtime owner without an ESP task stack."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[4]
FIXTURE=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pxa_esp_install_approval.h"
#include "pxa_esp_ui_shell.h"
#include "pxa/pxa_esp_storage.h"
#include "pxa/pxa_host.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
struct {uint32_t next_permission_prompt_id;struct {char active_identity[130];} activation;
    bool cooperative_active,graceful_stop_active,shutdown_complete;char deferred_launch_identity[130];} g_host{};
void *g_native_move,*g_store_install;
struct {uint32_t prompt_id;} g_store_result{},g_store_uninstall{};
typedef struct pxa_native_file pxa_native_file_t;
static pxa_native_file_t *g_native_file;
bool refreshed_pair_fits=true;
bool ime_active,resource_mode,post_ok=true,progress_ok=true,prepare_progress_ok=true,allocation_ok=true;
unsigned prepared,committed,closed,refs,reserved,modal,toasts,notified,dismissed;
size_t last_app,last_media;
pxa_status_t prepare_status=0,commit_status=0,last_result=999;
pxa_esp_ui_install_prompt_t posted{};
struct Allocation {void *pointer;template<class T> operator T*() const {return static_cast<T*>(pointer);}};
Allocation heap_caps_calloc(size_t n,size_t bytes,unsigned caps) {
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return {allocation_ok?calloc(n,bytes):nullptr};
}
void copy_utf8_c_string(char *out,size_t size,const char *in) {snprintf(out,size,"%s",in);}
bool pxa_esp_host_locale_is_chinese() {return true;}
bool pxa_esp_ime_active() {return ime_active;}
void pxa_esp_surface_runtime_modal_enter() {assert(!modal);++modal;}
void pxa_esp_surface_runtime_modal_leave() {assert(modal==1);--modal;}
void pxa_esp_ui_shell_post_toast(const char *text,uint32_t ms) {assert(text && ms);++toasts;}
int pxa_esp_ui_shell_post_install_prompt(const pxa_esp_ui_install_prompt_t *p) {assert(modal==1);posted=*p;return post_ok;}
void pxa_esp_ui_shell_dismiss_install_prompt(uint32_t id) {assert(id);++dismissed;}
int pxa_esp_ui_shell_post_file_prepare_progress(uint32_t id,const char *name) {
    assert(modal==1 && g_native_file && id && !strcmp(name,"package.pxa"));return prepare_progress_ok;
}
int pxa_esp_ui_shell_post_file_install_progress(uint32_t id,const char *name,uint8_t resources) {
    assert(modal==1 && id==posted.prompt_id && name[0] && resources==resource_mode);return progress_ok;
}
void pxa_esp_ui_shell_post_file_install_result(uint32_t id,int32_t status) {assert(!modal && id==posted.prompt_id);last_result=status;}
void pxa_host_notify_catalog_changed() {assert(!modal);++notified;}
pxa_status_t pxa_esp_storage_volume(uint64_t epoch,pxa_volume_info_t *volume) {
    memset(volume,0,sizeof(*volume));volume->mount_epoch=epoch;volume->state=PXA_VOLUME_READY;
    volume->space_known=PXA_VOLUME_FREE_KNOWN;volume->free_bytes=3000;volume->reserved_bytes=250;
    return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_package_store_evaluate_install(const pxa_esp_install_approval_t*,size_t app,size_t media,pxa_install_plan_t *plan) {
    if(!refreshed_pair_fits)return PXA_STATUS_QUOTA_EXCEEDED;
    plan->volumes[0].bytes=900+app+media;return PXA_STATUS_OK;
}
pxa_status_t pxa_esp_file_install_prepare(pxa_esp_file_install_t *file) {
    ++prepared;assert(modal==1 && g_native_file && !strcmp(file->source,"/sd/package.pxa") && !file->source_epoch);
    file->source_epoch=55;++refs;
    if(prepare_status)return prepare_status;
    auto &p=file->placement;p.count=2;p.selected=1;p.media_count=2;p.media_selected=0;
    p.requires_resources=1;p.media_fit[0]=3;p.media_fit[1]=1;
    for(unsigned i=0;i<2;++i) {
        strcpy(p.choices[i].volume.name,i?"SD":"Flash");p.choices[i].volume.mount_epoch=11+i;
        p.choices[i].estimate.volumes[0].bytes=500+i;p.choices[i].volume.free_bytes=1000;
        p.media_choices[i]=p.choices[i];p.media_choices[i].reuse=1;
    }
    strcpy(file->preview.name,"Game");strcpy(file->preview.version,"1.0");strcpy(file->preview.id,"publisher:game");
    file->resources_only=resource_mode;return 0;
}
void pxa_esp_file_install_close(pxa_esp_file_install_t *file) {
    ++closed;if(file->source_epoch) {assert(refs);--refs;file->source_epoch=0;}
    if(file->placement.plan.reserved) {assert(reserved);--reserved;file->placement.plan.reserved=0;}
}
pxa_status_t pxa_esp_file_install_commit(pxa_esp_file_install_t *file,size_t app,size_t media) {
    assert(refs==1 && modal==1);last_app=app;last_media=media;
    if(app>=file->placement.count || (resource_mode?media!=0:media>=2))return PXA_STATUS_INVALID_ARGUMENT;
    ++committed;++reserved;file->placement.plan.reserved=1;return commit_status;
}
// PRODUCTION
static pxa_native_file_t *job() {auto *p=static_cast<pxa_native_file_t*>(calloc(1,sizeof(pxa_native_file_t)));assert(p);strcpy(p->file.source,"/sd/package.pxa");return p;}
int main() {
    g_host.cooperative_active=true;native_file_prepare(job());assert(!prepared && !refs);g_host.cooperative_active=false;
    g_host.shutdown_complete=true;native_file_prepare(job());assert(!prepared);g_host.shutdown_complete=false;
    prepare_progress_ok=false;native_file_prepare(job());assert(!prepared && !g_native_file && !modal);prepare_progress_ok=true;
    native_file_prepare(job());assert(g_native_file && refs==1 && modal==1 && posted.selected==1 && posted.media_count==2);
    assert(posted.volumes[0].free_known && posted.volumes[0].free_bytes==2750);
    assert(posted.media_volumes[0].free_known && posted.media_volumes[0].free_bytes==2750);
    auto first=posted.prompt_id;native_file_decide(false,0);assert(!g_native_file && !refs && !modal && !committed);
    native_file_prepare(job());assert(posted.prompt_id>first);native_file_decide(true,0);
    assert(committed==1 && last_app==1 && last_media==0 && !refs && !reserved && !modal && !g_native_file);
    resource_mode=true;native_file_prepare(job());assert(posted.resources_only && !posted.media_count && posted.count==2);
    native_file_decide(true,1);assert(committed==2 && last_app==0 && last_media==0 && !refs && !reserved);
    resource_mode=false;native_file_prepare(job());native_file_prepare(job());assert(refs==1);native_file_cancel();assert(!refs);
    post_ok=false;native_file_prepare(job());assert(!g_native_file && !refs && !modal);post_ok=true;
    allocation_ok=false;native_file_prepare(job());assert(!g_native_file && !refs && !modal);allocation_ok=true;
    prepare_status=PXA_STATUS_DENIED;native_file_prepare(job());assert(!g_native_file && !refs);prepare_status=0;
    native_file_prepare(job());strcpy(g_host.activation.active_identity,"other");native_file_decide(true,0);
    assert(!g_native_file && !refs && committed==2);g_host.activation={};
    progress_ok=false;native_file_prepare(job());native_file_decide(true,0);
    assert(!refs && !reserved && committed==2 && last_result==PXA_STATUS_RESOURCE_LIMIT);progress_ok=true;
    commit_status=PXA_STATUS_BAD_STATE;native_file_prepare(job());native_file_decide(true,0);
    assert(!refs && !reserved && committed==3 && last_result==PXA_STATUS_BAD_STATE && !modal);
    native_file_prepare(job());native_file_decide(true,255);assert(!refs && !reserved && last_result==PXA_STATUS_INVALID_ARGUMENT);
    refreshed_pair_fits=false;native_file_prepare(job());
    assert(g_native_file && !posted.media_fit[0] && !posted.media_fit[1]);
    native_file_decide(false,0);assert(!refs && !reserved && !modal);
    assert(closed>=prepared && toasts && notified && dismissed);
    puts("Native Files owner: cancel, app/media selection, resources-only, stale activation, shutdown/cooperate rejection, allocation/post failures, reservation/source cleanup passed");
}
'''

def main():
    production=(ROOT/'firmware/components/pxa/src/runtime/pxa_host_file_install.inc').read_text()
    includes=['firmware/components/pxa/src/package','firmware/components/pxa/src/services','firmware/components/pxa/src/ui','firmware/components/pxa/include',
              'deps/pxa-system/libpxa/include','deps/pxa-system/libpxa/adapters/include']
    with tempfile.TemporaryDirectory(prefix='pxa-native-file-') as directory:
        source=Path(directory)/'file.cpp';binary=Path(directory)/'file';source.write_text(FIXTURE.replace('// PRODUCTION',production))
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-DESP_PLATFORM=1','-DCONFIG_PXA_ENABLED=1',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',*[f'-I{ROOT/path}' for path in includes],str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__':main()
