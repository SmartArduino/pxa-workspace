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
struct Store { bool legacy_ready=true,transfer_cleanup_pending=false; void* transaction_lock=(void*)3; void* installer=(void*)4; } store;
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
constexpr uint8_t kNoSession=UINT8_MAX;
uint8_t s_mutation_owner=kNoSession,current_session=0;
struct {bool package_staging=false,legacy_receiving=false;} session;
constexpr char kPxaInboxRoot[]="pxa-state/inbox";
constexpr char kPxaReceivingPrefix[]="pxa-state/inbox/.receiving-";
auto& CurrentSession() {return session;}
uint8_t CurrentSessionIndex() {return current_session;}
int64_t s_next_package_cleanup_us=0,now_us=0;
int64_t esp_timer_get_time() { return now_us; }
void AbortFileUpload() { ++uploads_aborted; }
const char* kFsUploadTemporary="/pxa/.pxadb-upload";
int unlink(const char* path) { assert(!strcmp(path,kFsUploadTemporary));++unlinks;return 0; }
// TRANSPORT
int main() {
    // An occupied, unrelated transaction cannot turn an empty debug
    // handshake into an Inbox scan or a write fence.
    for (int i=0;i<1000;++i) {
        AbortTransfers();
        assert(PackageTransfersReady("FSINSTALL",nullptr));
        TryDiscardPackageTransfers();
    }
    assert(!PackageCleanupPending() && attempts==0 && discards==0 && unlinks==0);
    AbortTransfers(true);assert(unlinks==1 && discards==0);

    assert(!TrackInboxStaging("pxa-state/inbox-extra/.receiving-bad"));
    assert(!session.package_staging && !session.legacy_receiving);
    assert(TrackInboxStaging("pxa-state/inbox"));
    assert(TrackInboxStaging("pxa-state/inbox/ready.pxa"));
    assert(session.package_staging && !session.legacy_receiving);
    AbortTransfers();assert(!session.package_staging && !discards && !attempts);
    session.package_staging=true; // e.g. volume-only install or progress UI
    AbortTransfers();assert(!session.package_staging && !discards && !attempts);

    // Simulate a failed boot recovery using the production sweep wrapper.
    locked=true;cleanup_status=-10;assert(discard_receiving_entries()==-10);locked=false;
    assert(PackageCleanupPending() && !s_package_cleanup_pending && discards==1);
    cleanup_status=0;TryDiscardPackageTransfers();assert(attempts==1 && discards==1);
    for(const char* command : {"HELLO","PING","INFO","PACKAGES","LOGSUB","FSLIST","FSGET"})
        assert(PackageTransfersReady(command,nullptr));
    for(const char* operation : {"paths","run","stop"})
        assert(PackageTransfersReady("PACKAGE",operation));
    for(const char* command : {"FSPUT","FSINSTALL","FSDATA","FSDATA2","FSBULK","FSFAST","FSFETCH",
            "FSMKDIR","FSMOVE","FSCOPY","FSRM","FSRMTREE"})
        assert(!PackageTransfersReady(command,nullptr));
    for(const char* operation : {"publish","deploy","install","install-volume","receive-progress","cache-import"})
        assert(!PackageTransfersReady("PACKAGE",operation));
    assert(attempts==1); // busy command retries share the background backoff
    now_us=999999;available=true;TryDiscardPackageTransfers();
    assert(attempts==1 && PackageCleanupPending());
    now_us=1000000;TryDiscardPackageTransfers();
    assert(attempts==2 && discards==2 && !PackageCleanupPending() && !locked);
    assert(PackageTransfersReady("FSPUT",nullptr));

    // A different link cannot discard live receiving directories or unlink
    // the active transport's reserved upload file.
    s_mutation_owner=1;
    AbortTransfers(true);assert(s_mutation_owner==1 && unlinks==1 && discards==2);
    s_package_cleanup_pending=true;TryDiscardPackageTransfers();
    assert(attempts==2 && PackageCleanupPending());
    current_session=1;assert(TrackInboxStaging("pxa-state/inbox/.receiving-test/manifest.pxm"));
    assert(session.legacy_receiving && TrackInboxStaging("pxa-state/inbox/ready.pxa") && session.legacy_receiving);
    self_owner=true;AbortTransfers();
    assert(s_mutation_owner==kNoSession && !session.package_staging && PackageCleanupPending());
    self_owner=false;cleanup_status=-10;now_us+=1000000;TryDiscardPackageTransfers();
    assert(PackageCleanupPending() && discards==3 && !locked);
    cleanup_status=0;now_us+=1000000;
    assert(PackageTransfersReady("PACKAGE","deploy") && discards==4 && !locked);

    // An unavailable legacy Host must leave recovery access to unrelated files.
    initialized=false;assert(TrackInboxStaging("pxa-state/inbox/.receiving-demo"));AbortTransfers();
    assert(!PackageCleanupPending() && discards==4);
    initialized=true;store.legacy_ready=false;assert(TrackInboxStaging("pxa-state/inbox/.receiving-demo"));AbortTransfers();
    assert(!PackageCleanupPending() && discards==4);
    store.legacy_ready=true;g_host_ready=false;assert(TrackInboxStaging("pxa-state/inbox/.receiving-demo"));AbortTransfers();
    assert(!PackageCleanupPending() && discards==4);
    puts("PXADB cleanup: busy boot owner never blocks connection; retry paced; new uploads fenced until old session drains; failures retained");
}
'''


def main():
    package = (ROOT/'firmware/components/pxa/src/package/pxa_esp_package_store.c').read_text()
    host = (ROOT/'firmware/components/pxa/src/runtime/pxa_host.c').read_text()
    transport = (ROOT/'firmware/components/pxadb/src_pxa_service.cc').read_text()
    source = FIXTURE.replace('// STORE', '\n'.join(function(package, signature) for signature in (
        'bool pxa_esp_package_store_transfer_cleanup_pending(', 'static pxa_status_t discard_receiving_entries(',
        'pxa_status_t pxa_esp_package_store_try_discard_transfers(')))
    source = source.replace('// HOST', '\n'.join(function(host, signature) for signature in (
        'bool pxa_host_package_transfer_cleanup_pending(', 'int32_t pxa_host_try_discard_package_transfers(')))
    source = source.replace('// TRANSPORT', '\n'.join(function(transport, signature) for signature in (
        'bool IsPxaInboxPath(', 'bool TrackInboxStaging(', 'bool PackageCleanupPending(', 'void TryDiscardPackageTransfers(', 'bool PackageTransfersReady(', 'void AbortTransfers(')))
    with tempfile.TemporaryDirectory(prefix='pxadb-cleanup-') as temporary:
        unit = Path(temporary)/'cleanup.cpp'
        binary = Path(temporary)/'cleanup'
        unit.write_text(source)
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
