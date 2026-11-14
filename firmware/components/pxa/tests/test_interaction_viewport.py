#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the real C99 registry with static, rotating and concurrent sources."""
import json,os
from pathlib import Path
import subprocess,sys,tempfile
from datetime import datetime

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT/'deps/pxa-system/tools/package'))
from interaction_contract import validate_device

def main():
    requested=os.environ.get('PXA_TEST_ARTIFACTS')
    artifacts=Path(requested) if requested else ROOT/'local/artifacts/interaction-viewport'/datetime.now().strftime('%Y%m%d-%H%M%S')
    artifacts.mkdir(parents=True,exist_ok=bool(requested))
    with tempfile.TemporaryDirectory(prefix='viewport-',dir=artifacts) as directory:
        work=Path(directory);binary=work/'test'
        command=[os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
            '-fno-omit-frame-pointer','-pthread','-I'+str(ROOT/'firmware/components/pxa/include'),
            str(Path(__file__).with_name('interaction_viewport_test.c')),
            str(ROOT/'firmware/components/pxa/src/runtime/pxa_host_interaction.c'),'-o',str(binary)]
        subprocess.run(command,check=True)
        outcomes=[]
        for scenario in ['static','null-query','live']+['race']*24:
            result=subprocess.run([str(binary),scenario],capture_output=True,text=True,check=True)
            snapshots=[validate_device(json.loads(line)) for line in result.stdout.splitlines() if line.startswith('{')]
            outcomes.append({'scenario':scenario,'snapshots':len(snapshots),'exit_code':result.returncode})
            if scenario=='live':
                assert [(x['display']['width'],x['display']['height']) for x in snapshots]==[(296,240),(240,296),(296,240),(240,296),(296,240)]
        (artifacts/'viewport-results.json').write_text(json.dumps({'passed':True,'compiler_command':command,'runs':outcomes,'scope':'Actual C API ASan/UBSan; no firmware/Guest or devices'},indent=2)+'\n')
    print('C99 interaction viewport: static fallback, rotations, failure/recovery, once-only APIs and24 concurrent publication races passed')

if __name__=='__main__':main()
