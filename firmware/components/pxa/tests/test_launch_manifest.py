#!/usr/bin/env python3
"""Exercise the real launch loader and parser, including ownership failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[4]
SDK=ROOT/'deps/pxa-system'
sys.path.insert(0,str(ROOT/'firmware/components/pxa_integration/tests'))
from test_startup import function
sys.path.insert(0,str(SDK/'tools/package'))
from build_package_manifest import main as build_manifest
parser=argparse.ArgumentParser();parser.add_argument('--libpxa',required=True);args=parser.parse_args()
component=ROOT/'firmware/components/pxa'
source=(component/'src/package/pxa_esp_package_store.c').read_text()
bodies='\n'.join(function(source[source.index("static size_t manifest_size_at_root("):],sig) for sig in (
    'static size_t manifest_size_at_root(', 'static int read_manifest_file(',
    'static int manifest_workspace_for_encoded(', 'static int load_manifest_file(',
    'static int manifest_matches_app_id(', 'static int manifest_publisher_root(',
    'static bool load_installed(', 'bool pxa_esp_package_store_prepare_launch('))
with tempfile.TemporaryDirectory(prefix='pxa-launch-') as temporary:
    temp=Path(temporary);package=temp/'package';(package/'artifacts').mkdir(parents=True)
    (package/'artifacts/main.esp32-s31.aot').write_bytes(b'opaque-aot-fixture')
    key=temp/'key.pem'
    subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',str(key)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    public=subprocess.check_output(['openssl','pkey','-in',str(key),'-pubout','-outform','DER'])
    (temp/'publisher.bin').write_bytes(hashlib.sha256(public).digest())
    metadata=temp/'package.json';metadata.write_text(json.dumps({'format':'pxa-package-source-0.1','id':'launch-fixture','version':'1','components':[{'id':'main','kind':'ui','artifact':'aot','services':['log']}]}))
    build_manifest([str(metadata),str(package),str(key),'esp32-s31','fixture'])
    fixture=temp/'fixture.c';fixture.write_text((component/'tests/pxa_esp_launch_manifest_test.c').read_text().replace('// PRODUCTION',bodies))
    includes=[component/'src/package',component/'src/services',component/'include',SDK/'libpxa/include',SDK/'libpxa/adapters/include']
    flags=['-std=c99','-Wall','-Wextra','-Werror','-g']
    if os.environ.get('PXA_TEST_SANITIZERS')=='1':flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    executable=temp/'test'
    subprocess.run(['cc',*flags,*('-I'+str(p) for p in includes),str(fixture),str(Path(args.libpxa).resolve()),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],cwd=temp,check=True)
