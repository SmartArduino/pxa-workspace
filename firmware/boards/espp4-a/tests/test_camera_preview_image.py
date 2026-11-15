#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline DMA-image ownership test using actual BSP/Core bodies and fake hardware."""
from pathlib import Path
import datetime,json,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[4]

def body(source,signature):
    assert source.count(signature)==1,signature
    start=source.index(signature);first=source.index('{',start);depth=0
    for end in range(first,len(source)):
        if source[end]=='{':depth+=1
        elif source[end]=='}':
            depth-=1
            if not depth:return source[start:end+1]
    raise AssertionError('Unclosed method')

def main():
    requested=os.environ.get('PXA_TEST_ARTIFACTS')
    artifacts=Path(requested) if requested else ROOT/'local/artifacts/camera-preview-image'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    artifacts.mkdir(parents=True,exist_ok=bool(requested));board=ROOT/'firmware/boards/espp4-a'
    source=(board/'src/espp4_a_camera.cc').read_text()
    methods=body(source,'void* AllocateImageResource(size_t bytes)')+'\n'+body(source,'void ReleaseImageResource(void* pointer)')+'\n'
    start=source.index('const espp4_a::CameraImageAllocator kImageAllocators[]');end=source.index('};',start)+2
    methods+=source[start:end]+'\n'+body(source,'pxa_status_t MakePreview(const uint16_t* source, pxa_raster_asset_t** output)')
    core=ROOT/'deps/pxa-system/libpxa';flags=['-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    includes=['-I'+str(core/'include'),'-I'+str(board/'include')];commands=[]
    with tempfile.TemporaryDirectory(prefix='actual-preview-',dir=artifacts) as directory:
        work=Path(directory);fixture=work/'test.cc'
        fixture.write_text(Path(__file__).with_name('camera_preview_image_test.cc.in').read_text().replace('// PRODUCTION_BODIES',methods))
        for name in ['asset_object','resource_budget']:
            commands.append([os.environ.get('CC','cc'),'-std=c99',*flags,*includes,'-c',str(core/'src/services/assets'/(name+'.c')),'-o',str(work/(name+'.o'))])
        commands.append([os.environ.get('CXX','c++'),'-std=c++20',*flags,*includes,str(fixture),str(work/'asset_object.o'),str(work/'resource_budget.o'),'-o',str(work/'test')])
        commands.append([str(work/'test')]);passed=False
        with (artifacts/'run.log').open('w') as log:
            for command in commands:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
            passed=True
        (artifacts/'result.json').write_text(json.dumps({'passed':passed,'commands':commands,'scope':'Actual BSP MakePreview plus actual Core image/resourcebudget with fake PPA/cache; hardware coherence/performance require separate device tests'},indent=2)+'\n')
    print((artifacts/'run.log').read_text().strip())

if __name__=='__main__':main()
