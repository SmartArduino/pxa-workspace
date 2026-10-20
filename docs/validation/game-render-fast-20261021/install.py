from pathlib import Path
import sys,os,json,subprocess,hashlib
sys.path[:0]=['tools','tools/pxadb','tools/game-perf']
import pxadb
from usb_keep_awake import usb_parent
from run_main_loop_probe import require_idle_packages
package=Path(sys.argv[1]);out=Path(sys.argv[2]);expected=sys.argv[3];out.mkdir(exist_ok=False)
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
assert subprocess.run(['fuser',str(Path(port).resolve())],capture_output=True).returncode==1
p=usb_parent(port);assert [(p/x).read_text().strip() for x in ['idVendor','idProduct','serial']]==['1a86','55d3','5CFF043509']
node=f"/dev/bus/usb/{int((p/'busnum').read_text()):03}/{int((p/'devnum').read_text()):03}"
os.environ['PXADB_BAUD']='2000000'
with open(node,'rb',buffering=0):
 with pxadb.PxaDbClient(port,10) as c:
  c.raw_callback=lambda raw:None
  hello=c.hello();assert pxadb.info_properties(hello)['serial']=='1c:29:04:d0:af:5e'
  info=pxadb.info_properties(c.request('INFO',timeout=30)[-1].payload);assert info['elf_sha256']==expected
  pkgs=c.request('PACKAGES',timeout=30);require_idle_packages(pkgs)
  files=list(package.rglob('*')) if package.is_dir() else [package]
  (out/'preflight.json').write_text(json.dumps({'hello':hello,'info':info,'packages':[f.payload for f in pkgs],'files':{str(f):{'bytes':f.stat().st_size,'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in files if f.is_file()}},indent=2)+'\n')
 with (out/'install.txt').open('w') as log:
  r=subprocess.run(['python','tools/pxadb/pxadb.py','package','install',str(package),'--port',port,'--baud','2000000','--timeout','60','--yes'],stdout=log,stderr=subprocess.STDOUT)
 print('install',r.returncode,flush=True);raise SystemExit(r.returncode)
