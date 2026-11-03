#!/usr/bin/env python3
"""Exercise the production Store self-update handoff before native commit."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "firmware/components/pxa_integration/tests"))
from test_startup import function

FIXTURE = r'''
#include "pxa/runtime.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#define PXA_ESP_HOST_TAG "test"
#define ESP_LOGI(...) ((void)0)
struct pxa_esp_store_job_t {
    uint64_t instance_id,request_token;
    pxa_component_t component;
    unsigned approved,cancelled,separate,self_update,commit_waiting;
    void *worker;
    struct { char id[130]; } preview;
} job,*g_store_install;
struct {
    struct {pxa_runtime_t *runtime;uint64_t active_instance_id;char active_identity[130];} activation;
    unsigned graceful_stop_active;
    void *retired_storage;
} g_host;
unsigned wakes,stops;
bool request_active;
static void xTaskNotifyGive(void *worker) {assert(worker==&job);++wakes;}
static int pxa_request_is_active(pxa_runtime_t *runtime,pxa_component_t component,uint64_t token) {
    assert(runtime==g_host.activation.runtime && component==7 && token==17);return request_active;
}
static void begin_graceful_stop(pxa_stop_reason_t reason,uint8_t after_back,uint32_t timeout) {
    assert(reason==PXA_STOP_REPLACED && !after_back && timeout==1200);++stops;g_host.graceful_stop_active=1;
}
// PRODUCTION
static void reset() {
    job={};g_host={};g_store_install=&job;wakes=stops=0;request_active=true;
    job.instance_id=42;job.request_token=17;job.component=7;job.approved=1;job.worker=&job;
    strcpy(job.preview.id,"publisher:pxa-store");
    strcpy(g_host.activation.active_identity,job.preview.id);
    g_host.activation.runtime=reinterpret_cast<pxa_runtime_t *>(1);g_host.activation.active_instance_id=42;
}
int main() {
    reset();prepare_store_install_commit(&job);
    assert(job.self_update && job.commit_waiting && !job.cancelled && stops==1 && !wakes);
    resume_store_install_after_stop(43,PXA_STOP_REPLACED);assert(!wakes && job.commit_waiting);
    resume_store_install_after_stop(42,PXA_STOP_REPLACED);
    assert(wakes==1 && !job.commit_waiting && !job.cancelled);
    resume_store_install_after_stop(42,PXA_STOP_REPLACED);assert(wakes==1);
    for(auto reason : {PXA_STOP_NORMAL,PXA_STOP_FAULT,PXA_STOP_SHUTDOWN}) {
        reset();prepare_store_install_commit(&job);resume_store_install_after_stop(42,reason);
        assert(job.cancelled && wakes==1 && !job.commit_waiting);
    }
    reset();prepare_store_install_commit(&job);g_host.retired_storage=&job;
    resume_store_install_after_stop(42,PXA_STOP_REPLACED);assert(job.cancelled && wakes==1);
    reset();strcpy(job.preview.id,"publisher:other-app");prepare_store_install_commit(&job);
    assert(wakes==1 && !stops && !job.self_update && !job.cancelled);
    reset();strcpy(job.preview.id,"other-publisher:pxa-store");prepare_store_install_commit(&job);
    assert(wakes==1 && !stops && !job.self_update);
    reset();job.separate=1;prepare_store_install_commit(&job);assert(wakes==1 && !stops && !job.self_update);
    for(unsigned invalid=0;invalid<6;++invalid) {
        reset();
        switch(invalid) {
            case 0:job.approved=0;break;
            case 1:job.cancelled=1;break;
            case 2:g_host.activation.runtime=nullptr;break;
            case 3:job.instance_id=43;break;
            case 4:request_active=false;break;
            case 5:g_host.graceful_stop_active=1;break;
        }
        prepare_store_install_commit(&job);assert(job.cancelled && wakes==1 && !stops && !job.self_update);
    }
    reset();g_store_install=nullptr;prepare_store_install_commit(&job);assert(!stops && !wakes);
    prepare_store_install_commit(nullptr);resume_store_install_after_stop(42,PXA_STOP_REPLACED);
    puts("Store self-update: identity, approval, live owner, stop-before-commit, cancellation, shutdown and failed storage retirement passed");
}
'''


def main():
    production = (ROOT / "firmware/components/pxa/src/runtime/pxa_esp_host.c").read_text()
    source = FIXTURE.replace("// PRODUCTION", "\n".join(function(production, signature) for signature in (
        "void prepare_store_install_commit(", "void resume_store_install_after_stop(")))
    with tempfile.TemporaryDirectory(prefix="pxa-store-self-update-") as directory:
        unit, binary = Path(directory) / "update.cpp", Path(directory) / "update"
        unit.write_text(source)
        subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-I" + str(ROOT / "deps/pxa-system/libpxa/include"), str(unit), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
