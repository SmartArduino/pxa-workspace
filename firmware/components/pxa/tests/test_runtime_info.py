#!/usr/bin/env python3
"""Inventory must work without, and never dereference, an active Guest engine."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'firmware/components/pxa_integration/tests'))
from test_startup import function

FIXTURE = r'''
#define ESP_PLATFORM 1
#include "pxa/pxa_host.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#define PXA_ESP_HOST_MAX_SERVICES 2
#define PXA_ESP_HOST_TARGET "fixture-target"
#define PXSYS_WAMR_ENGINE_ABI "fixture-engine-abi"
#define PXA_FS_SERVICE_ID 5
/* Intentionally no engine field: inventory cannot inspect a retiring pointer. */
static struct { int initialized; } g_host;
static unsigned calls;
static uint32_t pxa_wamr_engine_build_artifact_kinds(void) {
    ++calls; return (1u << PXA_ARTIFACT_WASM) | (1u << PXA_ARTIFACT_AOT);
}
static void host_service_capabilities(pxa_package_activation_profile_t *profile,
    pxa_package_service_capability_t *services, uint64_t fs, uint32_t models) {
    assert(!fs && models == PXA_EXECUTION_MODEL_MAIN_LOOP);
    profile->services = services; profile->service_count = 2;
    profile->execution_models = models;
    services[0].service = PXA_FS_SERVICE_ID; services[1].service = 3;
}
// PRODUCTION
int main(void) {
    pxa_host_runtime_info_t info;
    memset(&info, 0xa5, sizeof(info));
    assert(!pxa_esp_host_runtime_info(NULL) && !pxa_esp_host_runtime_info(&info));
    assert(!calls && info.unknown_features == 0xa5a5a5a5u);
    g_host.initialized = 1;
    assert(pxa_esp_host_runtime_info(&info));
    assert(calls == 1 && info.activation.services == info.services);
    assert(info.activation.service_count == 2 && info.unknown_features == 1);
    assert(info.artifact_kinds == ((1u << PXA_ARTIFACT_WASM) | (1u << PXA_ARTIFACT_AOT)));
    assert(info.host.target.size == strlen(PXA_ESP_HOST_TARGET));
    assert(!memcmp(info.host.target.data, PXA_ESP_HOST_TARGET, info.host.target.size));
    assert(info.manifest_major == PXA_PACKAGE_MANIFEST_FORMAT_MAJOR);
    puts("Runtime inventory: no active engine, stable caller-owned records, unknown FS features and initialization guard passed");
}
'''

def main():
    source = (ROOT / 'firmware/components/pxa/src/runtime/pxa_esp_host.c').read_text()
    body = function(source, 'bool pxa_esp_host_runtime_info(')
    with tempfile.TemporaryDirectory(prefix='pxa-runtime-info-') as directory:
        work = Path(directory)
        unit, binary = work / 'test.c', work / 'test'
        unit.write_text(FIXTURE.replace('// PRODUCTION', body))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(ROOT / 'firmware/components/pxa/include'),
                        '-I' + str(ROOT / 'deps/pxa-system/libpxa/include'), str(unit), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
