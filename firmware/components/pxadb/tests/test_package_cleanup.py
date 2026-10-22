#!/usr/bin/env python3
"""Run production transport cleanup against an occupied package owner."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define CONFIG_PXA_ENABLED 1
using pxa_status_t=int;
using SemaphoreHandle_t=void*;
constexpr int PXA_STATUS_OK=0,PXA_STATUS_WOULD_BLOCK=1,pdTRUE=1;
struct Store { bool legacy_ready=true; void* transaction_lock=(void*)3; void* installer=(void*)4; } store;
Store* g_store=&store;
bool initialized=true,g_host_ready=true,available=false,self_owner=false,locked=false;
unsigned attempts=0,discards=0,uploads_aborted=0,unlinks=0;
int cleanup_status=PXA_STATUS_OK;
bool store_is_initialized() { return initialized; }
void* xTaskGetCurrentTaskHandle() { return (void*)1; }
void* xSemaphoreGetMutexHolder(void* lock) { assert(lock==store.transaction_lock);return self_owner?(void*)1:(void*)2; }
int xSemaphoreTake(void* lock,unsigned timeout) {
    assert(lock==store.transaction_lock && timeout==0 && !locked);++attempts;
    locked=available;return available?pdTRUE:0;
}
void release_lock(void* lock) { assert(lock==store.transaction_lock && locked);locked=false; }
int pxa_posix_installer_discard_transfers(void* installer) {
    assert(installer==store.installer && locked);++discards;return cleanup_status;
}
// STORE
// HOST
std::atomic<bool> s_package_cleanup_pending{false};
int64_t s_next_package_cleanup_us=0,now_us=0;
int64_t esp_timer_get_time() { return now_us; }
void AbortFileUpload() { ++uploads_aborted; }
const char* kFsUploadTemporary="/pxa/.pxadb-upload";
int unlink(const char* path) { assert(!strcmp(path,kFsUploadTemporary));++unlinks;return 0; }
// TRANSPORT
int main() {
    AbortTransfers(); // occupied boot recovery must not delay HELLO/logcat
    assert(s_package_cleanup_pending && attempts==1 && !discards);
    assert(uploads_aborted==1 && unlinks==1 && !locked);
    for(const char* command : {"HELLO","PING","INFO","LOGCAT","FSLIST","FSGET"})
        assert(PackageTransfersReady(command) && attempts==1);
    for(const char* command : {"PACKAGE","FSPUT","FSDATA","FSDATA2","FSMKDIR","FSMOVE","FSCOPY","FSRM","FSRMTREE"})
        assert(!PackageTransfersReady(command));
    unsigned tried=attempts;
    now_us=999999;available=true;TryDiscardPackageTransfers();
    assert(attempts==tried && s_package_cleanup_pending); // bounded retry rate
    now_us=1000000;TryDiscardPackageTransfers();
    assert(discards==1 && !s_package_cleanup_pending && !locked);
    assert(PackageTransfersReady("FSPUT")); // new upload cannot be erased later
    TryDiscardPackageTransfers();assert(discards==1);
    self_owner=true;AbortTransfers();assert(s_package_cleanup_pending && attempts==tried+1);
    self_owner=false;cleanup_status=-10;now_us+=1000000;TryDiscardPackageTransfers();
    assert(s_package_cleanup_pending && discards==2 && !locked);
    cleanup_status=0;assert(PackageTransfersReady("PACKAGE") && discards==3 && !locked);
    initialized=false;AbortTransfers();assert(!s_package_cleanup_pending && discards==3);
    initialized=true;store.legacy_ready=false;AbortTransfers();assert(!s_package_cleanup_pending && discards==3);
    store.legacy_ready=true;g_host_ready=false;AbortTransfers();assert(!s_package_cleanup_pending && discards==3);
    puts("PXADB cleanup: busy boot owner never blocks connection; retry paced; new uploads fenced until old session drains; failures retained");
}
'''


def main():
    package = (ROOT/'firmware/components/pxa/src/package/pxa_esp_package_store.c').read_text()
    host = (ROOT/'firmware/components/pxa/src/runtime/pxa_host.c').read_text()
    transport = (ROOT/'firmware/components/pxadb/src_pxa_service.cc').read_text()
    source = FIXTURE.replace('// STORE', function(package, 'pxa_status_t pxa_esp_package_store_try_discard_transfers('))
    source = source.replace('// HOST', function(host, 'int32_t pxa_host_try_discard_package_transfers('))
    source = source.replace('// TRANSPORT', '\n'.join(function(transport, signature) for signature in (
        'void TryDiscardPackageTransfers(', 'bool PackageTransfersReady(', 'void AbortTransfers(')))
    with tempfile.TemporaryDirectory(prefix='pxadb-cleanup-') as temporary:
        unit = Path(temporary)/'cleanup.cpp'
        binary = Path(temporary)/'cleanup'
        unit.write_text(source)
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
