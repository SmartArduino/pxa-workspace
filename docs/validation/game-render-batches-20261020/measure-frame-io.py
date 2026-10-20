import sys, os, time, json
from pathlib import Path
sys.path.insert(0,'tools/pxadb');import pxadb
os.environ['PXADB_BAUD']='2000000'
out=Path('local/game-render-quake-frame-io-device');out.mkdir(exist_ok=True)
report={}
ready_stamp=0.0
held_buttons=0
last_keepalive=0.0
with (out/'serial.jsonl').open('a',buffering=1) as log,pxadb.PxaDbClient('/dev/ttyACM0',10) as c:
 def rec(k,p):log.write(json.dumps({'time':time.monotonic(),'kind':k,'payload':p})+'\n')
 def on_log(f):
  global ready_stamp
  rec(f.kind,f.payload)
  if 'PXA QUAKE ready=1' in f.payload:ready_stamp=time.monotonic()
 c.log_callback=on_log
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
  report['hello']=c.hello();assert pxadb.info_properties(report['hello'])['serial']=='1c:29:04:d0:af:5e'
  print([(f.kind,f.payload)for f in req('PACKAGES')],flush=True)
  c.subscribe_logs()
  req('INPUT KEY HOME');pump(.3)
  req('INPUT POINTER DOWN 160 185 0');pump(.15)
  for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.15)
  req('INPUT POINTER UP 160 53 0');pump(.4)
  req('PACKAGE run pxa-quake-sd');pump(25)
  shot('launch')
  button(64);pump(1);shot('start')
  button(16);pump(1);shot('singleplayer')
  button(16);pump(18);shot('newgame')
  for retry in range(20):
   if time.monotonic()-ready_stamp < 8:break
   pump(1)
  assert time.monotonic()-ready_stamp < 8,'No qualified actual-level readiness record'
  report['qualified_ready_before_measurement']=True
  samples=[];begin=time.monotonic()
  for i in range(60):
   pump(begin+i+1-time.monotonic());req('INPUT CONTROLLER 0 1 0')
   value=int(pxadb.info_properties(req('PERF FPS')[-1].payload)['presentation_fps_x10'])/10
   samples.append({'seconds':time.monotonic()-begin,'fps':value})
   if i%10==9:print('FRAME IO FPS',i+1,value,flush=True)
  report['fps_samples']=samples
  import statistics
  report['fps']={'median':statistics.median(x['fps'] for x in samples),'min':min(x['fps'] for x in samples),'max':max(x['fps'] for x in samples),'mean':statistics.mean(x['fps'] for x in samples)}
  report['ready_recent_at_end']=time.monotonic()-ready_stamp<8
  shot('measured')
  button(1,1.2);pump(2);shot('after-forward')
  button(8,.6);pump(2);shot('after-turn')
  for command in ['MEMORY','PERF FPS','PACKAGES']:
   c.unsubscribe_logs(timeout=5)
   report[command]=[{'kind':f.kind,'payload':f.payload}for f in req(command)]
   c.subscribe_logs()
   print(command,report[command],flush=True)
 finally:
  req('INPUT CONTROLLER 0 1 0')
  req('PACKAGE stop pxa-quake-sd')
  req('INPUT KEY HOME')
  (out/'measurement.json').write_text(json.dumps(report,indent=2))
