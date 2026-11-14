#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute actual BSP registration/viewport bodies with inert IDF/LVGL handles.

This is an offline source integration test, not a firmware or hardware run.
The actual C serializer and pinned rotation helper produce the tested JSON.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess,sys,tempfile
from datetime import datetime
ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'deps/pxa-system/tools/package'))
from interaction_contract import validate_device,evaluate

CAMERA={'schema':'pxa-app-interaction-1','profiles':[{'id':'touch','kind':'touch','complete':True,
    'requires':{'tap':'tap'},'contacts':1,'min_width':240,'min_height':200}]}

def body(source,signature):
    assert source.count(signature)==1,signature
    start=source.index(signature);first=source.index('{',start);depth=0
    # These bounded methods contain no braces in string/comment literals.
    for end in range(first,len(source)):
        if source[end]=='{':depth+=1
        elif source[end]=='}':
            depth-=1
            if not depth:return source[start:end+1]
    raise AssertionError('Unclosed production method')

PREFIX=r'''
#include "pxa/pxa_host_interaction.h"
#include "pxa/display_rotation.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
struct esp_app_desc_t { char project_name[32], version[32]; uint8_t app_elf_sha256[32]; };
static esp_app_desc_t image{};
static bool no_image;
static const esp_app_desc_t *esp_app_get_description() { return no_image ? nullptr : &image; }
struct ObservedRotation {
    uint8_t value=0; mutable unsigned reads=0;
    uint8_t load() const { ++reads; return value; }
};
'''

def harness(board,source,registration,display,contacts,width,height):
    cls='PaiTouchHardware' if board=='pai-touch' else 'Board'
    pointers='physical_pointers_' if board=='pai-touch' else 'pointers_'
    rotation='display_rotation_' if board=='pai-touch' else 'rotation_'
    constants=(f'#define PAI_DISPLAY_WIDTH {width}\n#define PAI_DISPLAY_HEIGHT {height}\n' if board=='pai-touch'
        else f'constexpr int kWidth={width}, kHeight={height};\n')
    definition=f'class {cls} {{ public: bool touch_ready_=true; unsigned touch_slots_={contacts}; void *{pointers}[{contacts}]{{}}; ObservedRotation {rotation}; bool RegisterInteraction(); '
    if board=='pai-touch':definition+='void DisplaySize(uint16_t*,uint16_t*) const; };\n'+display+'\n'
    else:definition+=display+' };\n'
    main=r'''
int main(int argc,char **argv) {
    assert(argc==2); std::string scenario=argv[1];
    std::strcpy(image.project_name,"test_firmware"); std::strcpy(image.version,"fixture-version");
    for(unsigned i=0;i<sizeof(image.app_elf_sha256);++i)image.app_elf_sha256[i]=i+1;
    BOARD board;
    for(auto &pointer:board.POINTERS)pointer=reinterpret_cast<void*>(uintptr_t(1));
    unsigned expected_contacts=CONTACTS;
    bool expected=true;
    if(scenario=="failed-touch") {board.touch_ready_=false;expected=false;}
    else if(scenario=="no-pointer0") {board.POINTERS[0]=nullptr;expected=false;}
    else if(scenario=="no-pointers") {for(auto &pointer:board.POINTERS)pointer=nullptr;expected=false;}
    else if(scenario=="gap") {board.POINTERS[1]=nullptr;expected_contacts=1;}
    else if(scenario=="slot2") {board.touch_slots_=2;expected_contacts=2;}
    else if(scenario=="slot0") {board.touch_slots_=0;expected=false;}
    else if(scenario=="empty-project") {image.project_name[0]=0;expected=false;}
    else if(scenario=="empty-version") {image.version[0]=0;expected=false;}
    else if(scenario=="unterminated-project") {std::memset(image.project_name,'x',sizeof(image.project_name));expected=false;}
    else if(scenario=="unterminated-version") {std::memset(image.version,'x',sizeof(image.version));expected=false;}
    else if(scenario=="zero-elf") {std::memset(image.app_elf_sha256,0,sizeof(image.app_elf_sha256));expected=false;}
    else if(scenario=="ff-elf") {std::memset(image.app_elf_sha256,255,sizeof(image.app_elf_sha256));expected=false;}
    else if(scenario=="no-image") {no_image=true;expected=false;}
    else assert(scenario=="healthy");
    assert(board.RegisterInteraction()==expected);
    char output[PXA_INTERACTION_MAX_BYTES+1];size_t size=999;
    if(!expected) {
        std::memset(output,0xa5,sizeof(output));
        assert(!pxa_host_interaction_describe(output,sizeof(output),&size));assert(size==0 && !output[0]);
        puts("unreported");return 0;
    }
    for(unsigned rotation=0;rotation<4;++rotation) {
        board.ROTATION.value=rotation;unsigned reads=board.ROTATION.reads;
        assert(pxa_host_interaction_describe(output,sizeof(output),&size));
        assert(size==std::strlen(output) && board.ROTATION.reads==reads+1);
        puts(output);
    }
    // Re-entering cannot rewrite borrowed contacts/identity/build storage.
    board.POINTERS[1]=nullptr;
    assert(!board.RegisterInteraction());
    assert(pxa_host_interaction_describe(output,sizeof(output),&size));
    char expected_count[64];std::snprintf(expected_count,sizeof(expected_count),"\"max_contacts\":%u",expected_contacts);
    assert(std::strstr(output,expected_count));puts(output);
}
'''
    for before,after in [('BOARD',cls),('POINTERS',pointers),('ROTATION',rotation),('CONTACTS',str(contacts))]:main=main.replace(before,after)
    return PREFIX+constants+definition+registration+'\n'+main

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--camera-package',type=Path);args=parser.parse_args()
    camera=json.loads(args.camera_package.read_text())['interaction'] if args.camera_package else CAMERA
    assert camera==CAMERA,'Keep this explicit Camera compatibility requirement source-bound'
    requested=os.environ.get('PXA_TEST_ARTIFACTS')
    artifacts=Path(requested) if requested else ROOT/'local/artifacts/board-interaction'/datetime.now().strftime('%Y%m%d-%H%M%S')
    artifacts.mkdir(parents=True,exist_ok=bool(requested));runs=[];bindings=[]
    with tempfile.TemporaryDirectory(prefix='board-registry-',dir=artifacts) as directory:
        work=Path(directory);obj=work/'registry.o'
        common=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run([os.environ.get('CC','cc'),'-std=c99',*common,'-I'+str(ROOT/'firmware/components/pxa/include'),
            '-c',str(ROOT/'firmware/components/pxa/src/runtime/pxa_host_interaction.c'),'-o',str(obj)],check=True)
        for board in ['pai-touch','espp4-a']:
            path=ROOT/f'firmware/boards/{board}/src'/('pai_touch_hardware.cc' if board=='pai-touch' else 'espp4_a_board.cc');source=path.read_text()
            cls='PaiTouchHardware' if board=='pai-touch' else 'Board'
            registration=body(source,f'bool {cls}::RegisterInteraction() {{')
            if board=='pai-touch':
                config=(ROOT/'firmware/boards/pai-touch/include/pai_touch_config.h').read_text()
                width=int(re.search(r'#define PAI_DISPLAY_WIDTH (\d+)',config)[1]);height=int(re.search(r'#define PAI_DISPLAY_HEIGHT (\d+)',config)[1])
                header=(ROOT/'firmware/boards/pai-touch/include/pai_touch_hardware.h').read_text();contacts=int(re.search(r'kTouchMaxPointers = (\d+)',header)[1])
                display=body(source,'void PaiTouchHardware::DisplaySize(uint16_t* width, uint16_t* height) const {')
            else:
                width,height=map(int,re.search(r'constexpr int kWidth = (\d+), kHeight = (\d+)',source).groups())
                header=(ROOT/'firmware/boards/espp4-a/include/espp4_a_touch.h').read_text();contacts=int(re.search(r'kTouchPointers = (\d+)',header)[1])
                display=body(source,'void DisplaySize(uint16_t* w, uint16_t* h) const {')
            assert (width,height,contacts)==((296,240,2) if board=='pai-touch' else (480,854,5))
            assert 'touch_ready_ = '+('InitializeTouch();' if board=='pai-touch' else 'Touch();') in source
            assert 'if (touch_ready_ && !RegisterInteraction())' in source
            cpp=work/(board+'.cc');cpp.write_text(harness(board,source,registration,display,contacts,width,height));exe=work/board
            command=[os.environ.get('CXX','c++'),'-std=c++20',*common,'-Wno-missing-field-initializers',
                '-I'+str(ROOT/'firmware/components/pxa/include'),'-I'+str(ROOT/'deps/pxa-system/libpxa/include'),str(cpp),str(obj),'-o',str(exe)]
            subprocess.run(command,check=True)
            bindings.append({'board':board,'production_source_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                'registration_body_sha256':hashlib.sha256(registration.encode()).hexdigest(),'viewport_body_sha256':hashlib.sha256(display.encode()).hexdigest(),'compiler_command':command})
            scenarios=['healthy','gap','failed-touch','no-pointer0','no-pointers','empty-project','empty-version',
                'unterminated-project','unterminated-version','zero-elf','ff-elf','no-image']
            if board=='espp4-a':scenarios+=['slot2','slot0']
            for scenario in scenarios:
                result=subprocess.run([str(exe),scenario],capture_output=True,text=True,check=True)
                if result.stdout.strip()=='unreported':
                    assert evaluate(camera,None)['status']=='unknown';runs.append({'board':board,'scenario':scenario,'descriptor':None,'Camera_status':'unknown'});continue
                documents=[validate_device(json.loads(line)) for line in result.stdout.splitlines()]
                expected_contacts=1 if scenario=='gap' else 2 if scenario=='slot2' else contacts
                for index,device in enumerate(documents):
                    assert device['physical']['max_contacts']==expected_contacts and device['physical']['touch'] is True
                    assert device['physical']['simultaneous'] is None
                    assert device['identity']['vendor']==device['identity']['hardware_revision']=='unknown'
                    assert device['identity']['firmware_version']=='fixture-version' and device['identity']['build_id']==bytes(range(1,33)).hex()
                    assert device['modes'][0]['actions']=={'tap':{'control':'touchscreen','events':['tap'],'hold_limit_ms':0,'tap_latency_ms':None}}
                    assert [(x['id']) for x in device['physical']['controls']]==['touchscreen']
                    rotation=index if index<4 else 3;expected=(height,width) if rotation&1 else (width,height)
                    assert (device['display']['width'],device['display']['height'])==expected
                    evaluation=evaluate(camera,device);assert evaluation['status']=='compatible' and evaluation['verified'] is False
                runs.append({'board':board,'scenario':scenario,'snapshots':len(documents),'actual_contacts':expected_contacts,'Camera_status':'compatible','hardware_verified':False})
    report={'passed':True,'scope':'Actual BSP registration+DisplaySize function bodies with inert hardware handles + actual C registry + pinned SDK contract; no fullfirmwarebuild/physicaldevice',
        'source_bindings':bindings,'runs':runs,'compiler':subprocess.check_output([os.environ.get('CXX','c++'),'--version'],text=True).splitlines()[0]}
    (artifacts/'board-interaction-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print('Pai/P4 interaction bodies: physical initialization/pointer-prefix/slots guards, immutable identity,4 rotations, SDK Camera compatibility and unknown failures passed')

if __name__=='__main__':main()
