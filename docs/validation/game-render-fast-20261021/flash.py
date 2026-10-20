from pathlib import Path
import sys,os,json,hashlib,subprocess,time
sys.path[:0]=['tools','tools/pxadb']
import pxadb
from usb_keep_awake import usb_parent
import argparse
ap=argparse.ArgumentParser();ap.add_argument('base',type=Path);ap.add_argument('out',type=Path);ap.add_argument('--from-elf',required=True);a=ap.parse_args()
out=a.out;out.mkdir(parents=True,exist_ok=True)
base=a.base
m=json.loads((base/'source-manifest.json').read_text())
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(base/'pxa_esp_platform.bin')==m['application_sha256']
assert sha(base/'pxa_esp_platform.elf')==m['elf_sha256']
assert (base/'pxa_esp_platform.bin').read_bytes()[176:208].hex()==m['elf_sha256']
part=(base/'partition_table/partition-table.bin').read_bytes()
backup=Path('build/game-native-pcm-20261020/flash-prefix-before.bin').read_bytes()
assert part==backup[0x8000:0x8000+len(part)]
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
assert subprocess.run(['fuser',str(Path(port).resolve())],capture_output=True).returncode==1
p=usb_parent(port)
assert [(p/x).read_text().strip() for x in ['idVendor','idProduct','serial']]==['1a86','55d3','5CFF043509']
node=f"/dev/bus/usb/{int((p/'busnum').read_text()):03}/{int((p/'devnum').read_text()):03}"
os.environ['PXADB_BAUD']='2000000'
with open(node,'rb',buffering=0):
 with pxadb.PxaDbClient(port,10) as c:
  c.raw_callback=lambda raw:None
  h=c.hello();assert pxadb.info_properties(h)['serial']=='1c:29:04:d0:af:5e'
  info=c.request('INFO',timeout=30)
  assert pxadb.info_properties(info[-1].payload)['elf_sha256']==a.from_elf
  before=c.request('PACKAGES',timeout=30)
  active=[f.payload.split('\t')[0].rsplit(':',1)[-1] for f in before if f.kind=='PKG' and 'active=1' in f.payload]
  assert active == [],active
  c.request('INPUT KEY HOME',timeout=30)
  (out/'flash-preflight.json').write_text(json.dumps({'hello':h,'info':[f.payload for f in info],'before':[f.payload for f in before],'stopped':active},indent=2)+'\n')
 with (out/'flash.txt').open('w') as log:
  result=subprocess.run(['/home/lucinhu/esp/esp-idf-toolchain/python_env/idf6.2_py3.14_env/bin/python','-m','esptool','--chip','esp32s31','--port',port,'--baud','921600','--before','default-reset','--after','hard-reset','write-flash','0x10000',str(base/'pxa_esp_platform.bin')],stdout=log,stderr=subprocess.STDOUT)
 print('Application-only flash exit:',result.returncode, 'ELF:',m['elf_sha256'],flush=True)
 raise SystemExit(result.returncode)
