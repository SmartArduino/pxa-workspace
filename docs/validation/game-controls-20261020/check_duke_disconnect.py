import sys, os, time, json, re, traceback
from pathlib import Path
sys.path[:0]=['tools','tools/pxadb'];import pxadb
from usb_keep_awake import usb_parent
os.environ['PXADB_BAUD']='2000000'
out=Path('build/game-controls-20261020/duke-disconnect');out.mkdir(exist_ok=True)
report={}
ready_stamp=0.0
held_buttons=0
last_keepalive=0.0
launch_device_ms=0
pcm_ready=False
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
parent=usb_parent(port)
if [(parent/x).read_text().strip() for x in ['idVendor','idProduct','serial']]!=['1a86','55d3','5CFF043509']:raise RuntimeError('Wrong USB bridge')
node=f"/dev/bus/usb/{int((parent/'busnum').read_text()):03}/{int((parent/'devnum').read_text()):03}"
with open(node,'rb',buffering=0) as usb_guard,(out/'serial.jsonl').open('a',buffering=1) as log,pxadb.PxaDbClient(port,10) as c:
 def rec(k,p):log.write(json.dumps({'time':time.monotonic(),'kind':k,'payload':p})+'\n')
 def on_log(f):
  global ready_stamp, pcm_ready
  rec(f.kind,f.payload)
  stamp=re.match(r'^(\d+)\t',f.payload)
  if not stamp or int(stamp[1])<launch_device_ms:return
  if 'PXA DUKE INPUT ' in f.payload and re.search(r'mode=4(?: |\x1b|$)',f.payload):ready_stamp=time.monotonic()
  if 'PXA PCM source=11025Hz/2ch S16LE sink=48000Hz/1ch' in f.payload:pcm_ready=True
 c.log_callback=on_log
 c.raw_callback=lambda raw:rec('RAW',raw.decode('utf-8','replace'))
 def req(cmd):
  rec('COMMAND',cmd);fs=c.request(cmd,timeout=30)
  for f in fs:
   if f.kind!='DATA' or not cmd.startswith('SCREENSHOT'):rec(f.kind,f.payload)
  return fs
 def pump(sec):
  end=time.monotonic()+sec
  global last_keepalive
  while time.monotonic()<end:
   c.pump_logs()
   if time.monotonic()-last_keepalive>.8:
    req(f'INPUT CONTROLLER 0 1 {held_buttons}');last_keepalive=time.monotonic()
   time.sleep(.01)
 def shot(name):
  c.unsubscribe_logs(timeout=5)
  try:
   for retry in range(5):
    try:s=pxadb.screenshot_capture(req('SCREENSHOT'));break
    except pxadb.PxaDbError as e:
     if retry==4 or not pxadb.screenshot_retryable(e):raise
     pump(.3)
   pxadb.write_screenshot(s,out/(name+'.png'));report[name]=s.metadata
   if name in {'measured','touch-restored','controller-hidden','release-stays-hidden','other-controller-ignored','disconnect-restored','moved-forward','moved-backward'}:
    border=[(x,y) for y in (192,239) for x in range(268,320)]+[(x,y) for x in (268,319) for y in range(193,239)]
    white=sum(int.from_bytes(s.data[y*640+x*2:y*640+x*2+2],'little')==65535 for x,y in border)
    visible=name in {'touch-restored','disconnect-restored'}
    report.setdefault('controls_checks',{})[name]={'white':white,'pixels':len(border),'expected_visible':visible}
    if (white==len(border))!=visible:raise RuntimeError(('Unexpected FIRE overlay',name,white,len(border)))
   print(name,s.metadata,flush=True)
  finally:c.subscribe_logs()
 def button(bits,seconds=.3):
  global held_buttons
  held_buttons=bits
  end=time.monotonic()+seconds
  while True:
   req(f'INPUT CONTROLLER 0 1 {bits}');pump(.15)
   if time.monotonic()>=end:break
  held_buttons=0
  req('INPUT CONTROLLER 0 1 0');pump(.5)
 try:
  report['hello']=c.hello()
  if pxadb.info_properties(report['hello'])['serial']!='1c:29:04:d0:af:5e':raise RuntimeError('Wrong device')
  info=pxadb.info_properties(req('INFO')[-1].payload);expected=json.loads(Path('build/game-native-pcm-20261020/fixed-firmware.json').read_text())['elf_sha256']
  if info.get('elf_sha256')!=expected:raise RuntimeError('Running firmware differs')
  report['firmware_info']=info
  packages=req('PACKAGES')
  print([(f.kind,f.payload)for f in packages],flush=True)
  if not any(f.kind=='PKG' and ':pxa-duke3d-sd\t' in f.payload and '\t0.1.34\t' in f.payload for f in packages):raise RuntimeError('Expected controls package missing')
  if any(f.kind=='PKG' and ':pxa-duke3d-sd\t' in f.payload and 'active=1' in f.payload for f in packages):
   req('PACKAGE stop pxa-duke3d-sd');pump(1)
  c.subscribe_logs()
  req('INPUT KEY HOME');pump(.3)
  req('INPUT POINTER DOWN 160 185 0');pump(.15)
  for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.15)
  req('INPUT POINTER UP 160 53 0');pump(.4)
  launch_device_ms=int(pxadb.info_properties(req('INPUT SYNC')[-1].payload)['device_us'])//1000
  req('PACKAGE run pxa-duke3d-sd');pump(25)
  shot('launch')
  button(16,1.2);pump(1);shot('controller-hidden')
  pump(3);shot('release-stays-hidden')
  req('INPUT CONTROLLER 0 0 0');pump(2);shot('disconnect-restored')
  report['scope']='Display/input recovery check; no gameplay FPS claim'
  for command in ['MEMORY','PERF FPS','PACKAGES']:
   c.unsubscribe_logs(timeout=5)
   report[command]=[{'kind':f.kind,'payload':f.payload}for f in req(command)]
   c.subscribe_logs()
   print(command,report[command],flush=True)
 except BaseException as error:
  report['error']=repr(error)
  report['traceback']=traceback.format_exc()
  raise
 finally:
  for command in ['INPUT CONTROLLER 0 1 0','PACKAGE stop pxa-duke3d-sd','INPUT KEY HOME']:
   try:req(command)
   except Exception as error:report.setdefault('cleanup_errors',[]).append({'command':command,'error':repr(error)})
  (out/'measurement.json').write_text(json.dumps(report,indent=2))
