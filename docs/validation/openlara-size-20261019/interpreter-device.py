#!/usr/bin/env python3
"""Isolated OpenLara interpreter probe; no other app mutations or firmware writes."""
import argparse,hashlib,json,os,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/pxadb'));import pxadb
os.environ['PXADB_BAUD']='2000000';os.environ['PXADB_UART_UPLOAD_CHUNK']='1024'
PUB='17982acd08493944059713aee9a56135dd4a080ecaaf7aa77df11fdf3102d171'
APP='pxa-openlara-probe'
p=argparse.ArgumentParser();p.add_argument('action',choices=('admission','media','install','run-safe','cleanup','restore'));args=p.parse_args()
base=ROOT/'local/openlara-full';out=base/'interpreter-device';out.mkdir(exist_ok=True)
report={'mode':'interpreter diagnostic, not production AOT','action':args.action};messages=[];permission_done=False;warnings_handled=0
with (out/(args.action+'.jsonl')).open('w',buffering=1) as log,pxadb.PxaDbClient('/dev/ttyACM0',30) as c:
 def record(kind,payload):
  log.write(json.dumps({'t':time.monotonic(),'kind':kind,'payload':payload})+'\n')
  if 'OpenLara' in payload or 'exception' in payload.lower() or 'failed' in payload.lower():print(payload,flush=True)
  messages.append(payload)
 c.log_callback=lambda f:record(f.kind,f.payload)
 c.raw_callback=lambda b:record('RAW',b.decode('utf-8','replace'))
 def req(cmd,timeout=45):
  record('COMMAND',cmd);rows=c.request(cmd,timeout=timeout)
  for f in rows:
   if not cmd.startswith('SCREENSHOT') or f.kind!='DATA':record(f.kind,f.payload)
  return rows
 def pump(seconds):
  end=time.monotonic()+seconds
  while time.monotonic()<end:c.pump_logs();time.sleep(.01)
 def reboot():
  req('REBOOT');time.sleep(3);c._hello_completed=False;c.log_subscribed=False;c.serial.buffer.clear();c.serial.reset_input_buffer();c.pending_frames.clear()
  end=time.monotonic()+45
  while True:
   try:c.hello();break
   except pxadb.PxaDbError:
    if time.monotonic()>=end:raise
    time.sleep(1)
  assert pxadb.info_properties(c.device_info)['serial']=='1c:29:04:d0:af:5e'
  c.subscribe_logs()
  for _ in range(30):
   try:req('PACKAGES');break
   except pxadb.PxaDbRejectedError:pump(.5)
  pump(2)
 def shot(name):
  pxadb.write_screenshot(pxadb.screenshot_capture(req('SCREENSHOT JPEG')),out/(name+'.jpg'))
 def wait(text,timeout=90):
  global permission_done
  end=time.monotonic()+timeout
  while time.monotonic()<end:
   if args.action=='run-safe':handle_slow_callback()
   if any(text in m for m in messages):return
   if not permission_done and any('Runtime permission prompt queued:' in m and APP in m for m in messages):
    shot('audio-permission');req('INPUT TAP 211 183');req('INPUT SYNC');permission_done=True
   pump(.3);req('PING',10)
  raise RuntimeError('timeout: '+text)
 def assert_probe_active():
  active=[f.payload for f in req('PACKAGES') if f.kind=='PKG' and 'active=1' in f.payload]
  assert len(active)==1 and ':'+APP+'\t' in active[0],('probe is not the sole active app',active)
 def handle_slow_callback():
  global warnings_handled
  count=sum('awaiting user decision' in m for m in messages)
  if count>warnings_handled:
   assert_probe_active();shot('slow-callback-'+str(count));req('INPUT TAP 226 170');req('INPUT SYNC');warnings_handled=count
 hello=c.hello();assert pxadb.info_properties(hello)['serial']=='1c:29:04:d0:af:5e';report['hello']=hello
 packages=req('PACKAGES');report['packages_before']=[f.payload for f in packages if f.kind=='PKG']
 unexpected=[f for f in report['packages_before'] if 'active=1' in f and ':pxa-openlara\t' not in f and ':'+APP+'\t' not in f]
 assert not unexpected,'another app is active; no writes performed'
 c.subscribe_logs()
 source=base/'interpreter-probe/pxa-openlara'
 try:
  if args.action=='admission':
   report['storage']=[f.payload for f in req('INFO')]
   upload,install=pxadb.package_storage_required(source);report.update(directory_upload=upload,adopt_commit=128*1024,required=upload+128*1024)
   assert report['required']<=1040000,'does not fit even after archiving own old code'
  elif args.action=='media':
   app=APP.encode();key=hashlib.sha256(b'PXA-APPKEY-1'+bytes.fromhex(PUB)+struct.pack('<H',len(app))+app).hexdigest();dest='/sdcard/PXA/media/'+key
   fs=pxadb.NormalFsClient(c)
   try:fs.list(dest)
   except pxadb.PxaDbRejectedError:fs.mkdir(dest)
   media=ROOT/'local/pxa-apps/openlara/assets-simulator/game';manifest=json.loads((media/'data-manifest.json').read_text())
   names=[f['file'] for f in manifest['files'] if f['file']!='levels.idx']+['data-manifest.json','levels.idx']
   for name in names:fs.put(media/name,dest+'/'+name)
   report.update(app_media=dest,files=names)
  elif args.action=='install':
   old=ROOT/'local/openlara-original-compare/controller-release/pxa-openlara'
   assert old.is_dir() and (old/'signature.pxs').is_file(),'original restore package missing'
   try:req('PACKAGE stop pxa-openlara')
   except pxadb.PxaDbRejectedError as e:
    if str(e)!='package_not_running':raise
   req('PACKAGE archive pxa-openlara');reboot()
   report['after_own_archive']=[f.payload for f in req('INFO')]
   try:
    pxadb.stage_package_files(pxadb.NormalFsClient(c),source,APP,60,publish=False);req('PACKAGE deploy '+APP,90)
   finally:pxadb.clear_staged_source(c,APP)
   reboot();report['installed']=[f.payload for f in req('PACKAGES') if f.kind=='PKG']
  elif args.action=='cleanup':
   reboot()
  elif args.action in ('run-safe'):
   req('INPUT KEY HOME');pump(.3);req('INPUT SWIPE 160 205 160 30 300 12');pump(.5)
   req('MEMORY START');req('PACKAGE run '+APP);wait('OpenLara ready level=0',120);pump(2)
   if not permission_done and any('Runtime permission prompt queued:' in m and APP in m for m in messages):
    shot('audio-permission');req('INPUT TAP 211 183');req('INPUT SYNC');permission_done=True;pump(2)
   shot('title')
   req('INPUT TAP 180 220');pump(2);req('INPUT TAP 160 116');wait('OpenLara ready level=3',120);pump(5)
   if args.action=='run-safe':handle_slow_callback();pump(1);assert_probe_active()
   shot('city')
   report['memory']=[f.payload for f in req('MEMORY STOP') if f.kind=='DATA'];samples=[]
   for i in range(20):
    pump(1)
    if args.action=='run-safe':handle_slow_callback();assert_probe_active()
    samples.append([f.payload for f in req('PERF FPS') if f.kind in ('DATA','OK')])
   report.update(city_ready=True,perf_fps=samples,warnings_handled=warnings_handled,production_aot=False);shot('city-after')
  elif args.action=='restore':
   try:req('PACKAGE stop '+APP)
   except pxadb.PxaDbRejectedError as e:
    if str(e)!='package_not_running':raise
   if any(':'+APP+'\t' in f.payload for f in packages if f.kind=='PKG'):req('PACKAGE archive '+APP)
   reboot();old=ROOT/'local/openlara-original-compare/controller-release/pxa-openlara'
   current=[f.payload for f in req('PACKAGES') if f.kind=='PKG']
   if not any(':pxa-openlara\t' in row and 'installed=1' in row for row in current):
    try:
     pxadb.stage_package_files(pxadb.NormalFsClient(c),old,'pxa-openlara',60,publish=False);req('PACKAGE deploy pxa-openlara',90)
    finally:pxadb.clear_staged_source(c,'pxa-openlara')
    reboot()
   req('INPUT KEY HOME');pump(.4);req('INPUT SWIPE 160 205 160 30 300 12');pump(.4);req('PACKAGE run pxa-openlara');pump(8);shot('restored-027')
   report['packages_after']=[f.payload for f in req('PACKAGES') if f.kind=='PKG'];report['storage']=[f.payload for f in req('INFO')]
  report['passed']=True
 except BaseException as e:report['error']=str(e);raise
 finally:
  (out/(args.action+'.json')).write_text(json.dumps(report,indent=2)+'\n')
