import json,os,sys,subprocess,time
from pathlib import Path
sys.path[:0]=['tools','tools/pxadb']
import pxadb
from usb_keep_awake import usb_parent
key='duke'
ids={'quake':'pxa-quake-sd','quake2':'pxa-quake2-sd','duke':'pxa-duke3d-sd','cnc':'pxa-command-conquer-sd','ra':'pxa-red-alert-sd'}
resource={'quake':'quake','quake2':'quake2','duke':'duke3d','cnc':'command-conquer','ra':'red-alert'}
if key not in ids:raise RuntimeError('Unknown owned game')
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
p=usb_parent(port)
if [(p/x).read_text().strip() for x in ['idVendor','idProduct','serial']]!=['1a86','55d3','5CFF043509']:raise RuntimeError('Wrong USB')
node=f"/dev/bus/usb/{int((p/'busnum').read_text()):03}/{int((p/'devnum').read_text()):03}"
base=Path('build/game-native-pcm-20261020')
package=Path('build/game-controls-20261020/packages/pxa-duke-native-pcm.pxa')
expected='6f934a3bc9fe39a4c1a2de2d8881a01e6e98d03a3d016242af2d1b783bc0dffb'
if pxadb.file_sha256(package)!=expected:raise RuntimeError('Package differs')
os.environ['PXADB_BAUD']='2000000'
report={}
with open(node,'rb',buffering=0):
 with pxadb.PxaDbClient(port,10) as c:
  report['hello']=c.hello()
  if pxadb.info_properties(report['hello'])['serial']!='1c:29:04:d0:af:5e':raise RuntimeError('Wrong MCU')
  info=pxadb.info_properties(c.request('INFO')[-1].payload);report['info']=info
  if info['elf_sha256']!=json.loads((base/'fixed-firmware.json').read_text())['elf_sha256']:raise RuntimeError('Wrong firmware')
  plan=json.loads((base/'resource-plans'/(resource[key]+'.json')).read_text())
  report['resources']=[]
  for f in plan['files']:
   remote=plan['sd_root']+'/'+f['path']
   frames=c.request('FSSHA '+pxadb.client_encode_path(remote),timeout=pxadb.file_verification_timeout(f['bytes']))
   fields=pxadb.info_properties(frames[-1].payload)
   if fields.get('sha256')!=f['sha256'] or int(fields.get('bytes',-1))!=f['bytes']:raise RuntimeError(('Resource mismatch',remote,fields))
   report['resources'].append({'path':remote,**fields})
  packages=c.request('PACKAGES');report['packages_before']=[{'kind':f.kind,'payload':f.payload} for f in packages]
  if any(f.kind=='PKG' and ':pxa-command-conquer-sd\t' in f.payload and 'active=1' in f.payload for f in packages):
   c.request('PACKAGE stop pxa-command-conquer-sd',timeout=30)
   packages=c.request('PACKAGES')
  for attempt in range(40):
   active=[f.payload for f in packages if f.kind=='PKG' and 'active=1' in f.payload]
   if not active:break
   if any(':pxa-command-conquer-sd\t' not in row for row in active):raise RuntimeError(('Unexpected active app',active))
   time.sleep(.25);packages=c.request('PACKAGES')
  else:raise RuntimeError(('C&C stop still pending',active))
  report['archived']=[]
  for old in ids.values():
   if old!=ids[key] and any(f.kind=='PKG' and ':'+old+'\t' in f.payload for f in packages):
    c.request('PACKAGE archive '+old,timeout=30);report['archived'].append(old)
  report['info_after_archive']=c.request('INFO')[-1].payload
 Path('build/game-controls-20261020/duke-install-preflight.json').write_text(json.dumps(report,indent=2)+'\n')
 result=subprocess.run([sys.executable,'tools/pxadb/pxadb.py','package','install',str(package),'--port',port,'--baud','2000000','--identity',ids[key],'--yes'],check=True)
