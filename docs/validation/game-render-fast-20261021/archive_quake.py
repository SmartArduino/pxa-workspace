from pathlib import Path
import sys,os,json,subprocess
sys.path[:0]=['tools','tools/pxadb','tools/game-perf']
import pxadb
from usb_keep_awake import usb_parent
from run_main_loop_probe import require_idle_packages
report=Path(sys.argv[1]);out=Path(sys.argv[2])
d=json.loads(report.read_text());assert d['qualified'] and d['stopped']
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
assert subprocess.run(['fuser',str(Path(port).resolve())],capture_output=True).returncode==1
p=usb_parent(port);assert [(p/x).read_text().strip() for x in ['idVendor','idProduct','serial']]==['1a86','55d3','5CFF043509']
node=f"/dev/bus/usb/{int((p/'busnum').read_text()):03}/{int((p/'devnum').read_text()):03}"
os.environ['PXADB_BAUD']='2000000'
with open(node,'rb',buffering=0),pxadb.PxaDbClient(port,10) as c:
 c.raw_callback=lambda raw:None
 assert pxadb.info_properties(c.hello())['serial']=='1c:29:04:d0:af:5e'
 assert pxadb.info_properties(c.request('INFO')[-1].payload)['elf_sha256']==d['info']['elf_sha256']
 before=c.request('PACKAGES');require_idle_packages(before)
 frames=c.request('PACKAGE archive pxa-quake-sd',timeout=60)
 after=c.request('PACKAGES');assert not any(f.kind=='PKG' and ':pxa-quake-sd\t' in f.payload for f in after)
 result={'before':[f.payload for f in before],'archive':[{'kind':f.kind,'payload':f.payload} for f in frames],'after':[f.payload for f in after]}
 out.write_text(json.dumps(result,indent=2)+'\n');print('Archived owned Quake code; private data retained')
