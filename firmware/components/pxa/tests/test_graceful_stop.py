#!/usr/bin/env python3
"""Exercise production stop readiness, timeout, replacement and reentry."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE=r'''
#include "pxa/runtime.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#define PXA_ESP_HOST_MAX_APP_ID 130
#define PXA_HOST_SYSTEM_LIFECYCLE_BACKGROUND 0
#define PXA_HOST_RUNTIME_START_FAILED 1
struct Host {
    struct {pxa_component_t active_component;pxa_runtime_t *runtime;char active_identity[130];} activation;
    unsigned graceful_stop_active,cooperative_exit_pending,lifecycle_delivered,graceful_stop_after_back,shutdown_complete;
    pxa_stop_reason_t graceful_stop_reason;
    uint64_t shutdown_deadline_us;
    char deferred_launch_identity[130];
} g_host;
unsigned drains,queries,stops,backs,starts,failures,lock_depth;
uint64_t now;
bool exit_on_drain,start_ok;
pxa_status_t snapshot_status;
pxa_component_snapshot_t snapshot;
#define portENTER_CRITICAL(lock) (++lock_depth)
#define portEXIT_CRITICAL(lock) (--lock_depth)
pxa_status_t pxa_component_snapshot(const pxa_runtime_t *runtime,pxa_component_t component,pxa_component_snapshot_t *out) {
    assert(runtime && component==7);++queries;*out=snapshot;return snapshot_status;
}
uint64_t host_now_us(void*) {return now;}
void drain_active_events() {++drains;if(exit_on_drain)g_host.cooperative_exit_pending=1;}
void stop_active(pxa_stop_reason_t reason) {
    assert(reason==g_host.graceful_stop_reason);++stops;g_host.activation.active_identity[0]=0;
    g_host.activation.active_component=PXA_COMPONENT_INVALID;g_host.activation.runtime=nullptr;
    g_host.graceful_stop_active=0;g_host.deferred_launch_identity[0]=0;
}
void stop_active_after_back() {++backs;stop_active(g_host.graceful_stop_reason);}
void advance_graceful_stop();
bool start_verified(const char *identity) {
    assert(!strcmp(identity,"next") && !g_host.graceful_stop_active && !g_host.deferred_launch_identity[0]);++starts;
    advance_graceful_stop(); // A foreground callback may reenter the owner.
    return start_ok;
}
void notify_runtime_event(unsigned event,const char *identity,uint64_t instance,unsigned reason) {
    assert(event==PXA_HOST_RUNTIME_START_FAILED && !strcmp(identity,"next") && !instance && reason==PXA_STOP_NORMAL);++failures;
}
// PRODUCTION
void reset() {
    g_host={};g_host.activation.active_component=7;g_host.activation.runtime=reinterpret_cast<pxa_runtime_t*>(1);
    strcpy(g_host.activation.active_identity,"old");g_host.graceful_stop_active=1;g_host.lifecycle_delivered=1;
    g_host.shutdown_deadline_us=200;g_host.graceful_stop_reason=PXA_STOP_NORMAL;
    drains=queries=stops=backs=starts=failures=lock_depth=0;now=100;exit_on_drain=false;start_ok=true;snapshot_status=PXA_STATUS_OK;snapshot={};
}
int main() {
    reset();g_host.graceful_stop_active=0;advance_graceful_stop();assert(!drains && !queries && !stops);
    reset();advance_graceful_stop();assert(drains==1 && !queries && !stops && g_host.graceful_stop_active);
    g_host.lifecycle_delivered=0;snapshot.pending_requests=1;advance_graceful_stop();assert(queries==1 && !stops);
    snapshot.pending_requests=0;snapshot.queued_events=1;advance_graceful_stop();assert(queries==2 && !stops);
    snapshot.queued_events=0;snapshot_status=PXA_STATUS_BAD_STATE;advance_graceful_stop();assert(!stops);
    now=200;advance_graceful_stop();assert(stops==1 && !g_host.graceful_stop_active);
    reset();g_host.lifecycle_delivered=0;advance_graceful_stop();assert(stops==1 && queries==1 && !starts);
    reset();g_host.activation.active_component=PXA_COMPONENT_INVALID;g_host.activation.runtime=nullptr;
    g_host.activation.active_identity[0]=0;strcpy(g_host.deferred_launch_identity,"next");advance_graceful_stop();
    assert(starts==1 && !queries && !stops && !g_host.graceful_stop_active);
    reset();g_host.lifecycle_delivered=0;g_host.graceful_stop_after_back=1;strcpy(g_host.deferred_launch_identity,"next");
    start_ok=false;advance_graceful_stop();assert(stops==1 && backs==1 && starts==1 && failures==1);
    reset();g_host.graceful_stop_reason=PXA_STOP_SHUTDOWN;now=200;advance_graceful_stop();
    assert(stops==1 && g_host.shutdown_complete && !lock_depth);
    reset();exit_on_drain=true;advance_graceful_stop();assert(!queries && !stops && g_host.graceful_stop_active);
    puts("Graceful stop: background quiescence, pending work, failed snapshot, timeout, deferred launch, reentry, BACK and shutdown passed");
}
'''


def main():
    production=(ROOT/'firmware/components/pxa/src/runtime/pxa_esp_host.c').read_text()
    source=FIXTURE.replace('// PRODUCTION','\n'.join(function(production,s) for s in (
        'int graceful_stop_ready(', 'void advance_graceful_stop(')))
    with tempfile.TemporaryDirectory(prefix='pxa-graceful-stop-') as directory:
        unit=Path(directory)/'stop.cpp';binary=Path(directory)/'stop';unit.write_text(source)
        subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I'+str(ROOT/'deps/pxa-system/libpxa/include'),str(unit),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__=='__main__':main()
