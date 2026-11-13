#!/usr/bin/env python3
"""Compile the product registry and exercise its bounded, Guest-free serializer."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'tools/pxadb'))
from interaction_contract import validate_device

FIXTURE=r'''
#include "pxa/pxa_host_interaction.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const pxa_interaction_control_t controls[]={{"power","BOOT \"键\""}};
static const pxa_interaction_binding_t bindings[]={{"confirm","power",PXA_INTERACTION_TAP,0,-1}};
static const pxa_interaction_mode_t modes[]={{"focus","focus",bindings,1}};
static const pxa_interaction_reserved_t reserved[]={{"power","hold","system.menu",350}};
int main(void) {
    char output[PXA_INTERACTION_MAX_BYTES+1];size_t size=999;
    assert(!pxa_host_interaction_describe(output,sizeof(output),&size)&&size==0);
    assert(!pxa_host_interaction_register(NULL));
    static const pxa_host_interaction_t descriptor={
        .struct_size=sizeof(descriptor),.vendor="test",.model="test",.hardware_revision="unknown",
        .firmware_id="test",.firmware_version="1",.build_id="test",.profile_id="three-key",.revision=1,
        .width=240,.height=284,.shape="rounded",.touch=0,.simultaneous=-1,.max_contacts=0,
        .controls=controls,.control_count=1,.modes=modes,.mode_count=1,.reserved=reserved,.reserved_count=1
    };
    assert(pxa_host_interaction_register(&descriptor));
    assert(!pxa_host_interaction_register(&descriptor));
    for(size_t capacity=1;capacity<700;capacity++){
        memset(output,0xa5,sizeof(output));
        bool ok=pxa_host_interaction_describe(output,capacity,&size);
        assert((unsigned char)output[capacity]==0xa5);
        assert(ok ? size<capacity&&strlen(output)==size : size==0&&output[0]==0);
    }
    assert(pxa_host_interaction_describe(output,sizeof(output),&size));
    assert(size==strlen(output));puts(output);
}
'''


def main():
    artifacts=Path(os.environ.get('PXA_TEST_ARTIFACTS',ROOT/'local/artifacts/device-interaction'))
    artifacts.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='registry-',dir=artifacts) as directory:
        work=Path(directory);unit=work/'test.c';binary=work/'test';unit.write_text(FIXTURE)
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-I'+str(ROOT/'firmware/components/pxa/include'),str(unit),
            str(ROOT/'firmware/components/pxa/src/runtime/pxa_host_interaction.c'),'-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
        descriptor=validate_device(json.loads(result.stdout))
        assert descriptor['physical']['touch'] is False
        assert descriptor['modes'][0]['actions']['confirm']['tap_latency_ms'] is None
        assert descriptor['physical']['controls'][0]['label']=='BOOT "键"'
    print('Interaction registry: bounded UTF-8 JSON, buffer guards, absent/once-only registration, ASan/UBSan passed')


if __name__=='__main__':main()
