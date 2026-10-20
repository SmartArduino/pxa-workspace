import os,sys,time,json,statistics,hashlib
from pathlib import Path
os.environ['PXADB_BAUD']='2000000';sys.path.insert(0,'tools/pxadb');import pxadb
out=Path('local/quake-probe-device');out.mkdir(exist_ok=True)
control=Path('local/quake-probe-control.json')
probe_state={}; ready_stamp=0.0; position_device_stamp=0; latest_device_stamp=0; launch_marker=0
mode_path='sdcard/PXA/media/207f14f1edf22e79d1ee07571c729bc1bf71f3212e4007ec2905aca346552538/pxa-render-probe-mode'
attach='--attach'in sys.argv
launched=False
mode_exists=attach
report={'app':'pxa-quake-sd','windows':[],'actions':[]}
held=0;last_keep=0;done=False;processing=False
with (out/'serial.jsonl').open('w',buffering=1)as log,pxadb.PxaDbClient('/dev/ttyACM0',10)as c:
 def rec(k,p):
  global ready_stamp,probe_state,position_device_stamp,latest_device_stamp
  log.write(json.dumps({'time':time.monotonic(),'kind':k,'payload':p})+'\n')
  stamp=p.split('\t',1)[0]
  if stamp.isdigit():latest_device_stamp=max(latest_device_stamp,int(stamp))
  if 'PXA QUAKE 'in p and stamp.isdigit()and int(stamp)>max(launch_marker,position_device_stamp):
   position_device_stamp=int(stamp)
   probe_state={k:(v if k=='xyz'else int(v))for k,v in __import__('re').findall(r'(\w+)=([0-9,-]+)',p)}
   ready_stamp=time.monotonic() if probe_state.get('ready')==1 and probe_state.get('hp')==100 else 0.0

 c.log_callback=lambda f:rec(f.kind,f.payload)
 def save():
  (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 def req(cmd):
  rec('COMMAND',cmd);fs=c.request(cmd,timeout=30)
  for f in fs:
   if f.kind!='DATA'or not cmd.startswith('SCREENSHOT'):rec(f.kind,f.payload)
  return fs
 def pump(seconds,allow_control=True):
  global last_keep,processing
  end=time.monotonic()+max(0,seconds)
  while time.monotonic()<end:
   c.pump_logs()
   if time.monotonic()-last_keep>.8:
    req(f'INPUT CONTROLLER 0 1 {held}');last_keep=time.monotonic()
   if allow_control and not processing and control.exists():
    a=json.loads(control.read_text());control.unlink();processing=True
    try:action(a)
    finally:processing=False
   time.sleep(.01)
 def put_mode(mode):
  global mode_exists
  fs=pxadb.NormalFsClient(c)
  if mode_exists:
   fs.remove(mode_path);mode_exists=False
  local=out/'mode-byte';local.write_bytes(str(mode).encode())
  fs.put(local,mode_path);mode_exists=True
 def shot(name):
  c.unsubscribe_logs(timeout=5)
  try:
   for retry in range(5):
    try:s=pxadb.screenshot_capture(req('SCREENSHOT'));break
    except pxadb.PxaDbError as e:
     if retry==4 or not pxadb.screenshot_retryable(e):raise
     pump(.4,False)
   pxadb.write_screenshot(s,out/(name+'.png'));report.setdefault('screenshots',{})[name]=s.metadata
   print('SHOT',name,s.metadata,flush=True);save()
   return s.metadata
  finally:c.subscribe_logs()
 def action(a):
  global held,done
  print('ACTION',a,flush=True);report['actions'].append(a)
  if 'mode'in a:
   mode=a['mode'];assert mode in range(4)
   put_mode(mode)
   deadline=time.monotonic()+30
   while probe_state.get('mode')!=mode or time.monotonic()-ready_stamp>=8:
    assert time.monotonic()<deadline,(mode,probe_state)
    pump(.5,False)
  elif 'run_modes'in a:
   for index,mode in enumerate(a['run_modes']):
    action({'mode':mode})
    action({'measure':f'mode-{mode}-{index}','seconds':60,'warmup':20})
  elif 'tap'in a:
   x,y=a['tap'];buttons=a.get('buttons',0)
   req(f'INPUT POINTER DOWN {x} {y} {buttons}');pump(.25,False)
   req(f'INPUT POINTER UP {x} {y} {buttons}');pump(a.get('settle',1),False)
  elif 'button'in a:
   held=a['button'];req(f'INPUT CONTROLLER 0 1 {held}');pump(a.get('seconds',.5),False)
   held=0;req('INPUT CONTROLLER 0 1 0');pump(1,False)
  elif 'shot'in a:shot(a['shot'])
  elif 'measure'in a:
   name=a['measure'];seconds=a.get('seconds',60);samples=[]
   deadline=time.monotonic()+120
   while time.monotonic()-ready_stamp>=8:
    assert time.monotonic()<deadline,'No fresh actual-level record'
    pump(1,False)
   assert probe_state.get('xyz')=='544,288,50',probe_state
   state_before=dict(probe_state)
   before=shot(name+'-before');pump(a.get('warmup',30),False)
   start=time.monotonic()
   for i in range(seconds):
    pump(start+i+1-time.monotonic(),False)
    fs=req('PERF FPS');v=int(pxadb.info_properties(fs[-1].payload)['presentation_fps_x10'])/10
    samples.append({'seconds':time.monotonic()-start,'fps':v})
    if i%10==9:print('QUAKE PROBE FPS',name,i+1,v,flush=True)
   values=[x['fps']for x in samples]
   assert time.monotonic()-ready_stamp<8 and probe_state.get('xyz')=='544,288,50'and probe_state.get('mode')==state_before.get('mode'),probe_state
   row={'label':name,'qualified_actual_gameplay':False,'qualification_note':'Requires visual review of both actual mission screenshots; menu/loading never qualifies.','fps_samples':samples,'fps':{'median':statistics.median(values),'mean':statistics.mean(values),'min':min(values),'max':max(values)},'before':before,'after':shot(name+'-after'),'state_before':state_before,'state_after':dict(probe_state),'fresh_ready_before_and_after':True}
   report['windows'].append(row);save();print('WINDOW',name,row['fps'],flush=True)
  elif 'status'in a:
   for cmd in ['PACKAGES','MEMORY']:
    report[cmd]=[{'kind':f.kind,'payload':f.payload}for f in req(cmd)]
   print('STATUS',report.get('PACKAGES'),flush=True)
  elif 'finish'in a:done=True
  else:raise ValueError(a)
  save()
 try:
  report['hello']=c.hello();assert pxadb.info_properties(report['hello'])['serial']=='1c:29:04:d0:af:5e'
  ps=req('PACKAGES');active=[f.payload for f in ps if f.kind=='PKG'and 'active=1'in f.payload]
  assert all(':pxa-quake-sd\t'in p for p in active)if attach else not active
  report['packages_before']=[f.payload for f in ps if f.kind=='PKG']
  c.subscribe_logs()
  if attach:
   assert active
   launched=True;report['attached_existing_game']=True;report['launched_at']=time.monotonic();save()
   req('INPUT CONTROLLER 0 1 0');pump(10);shot('attach')
  else:
   req('INPUT KEY HOME');pump(.3)
   req('INPUT POINTER DOWN 160 185 0');pump(.15)
   for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.15)
   req('INPUT POINTER UP 160 53 0');pump(.4)
   put_mode(0)
   launch_marker=latest_device_stamp
   req('PACKAGE run pxa-quake-sd');launched=True;report['launched_at']=time.monotonic();save()
   pump(25);shot('launch')
  end=time.monotonic()+900
  while not done and time.monotonic()<end:pump(.5)
 except BaseException as error:
  report['failure']={'type':type(error).__name__,'message':str(error)}
  raise
 finally:
  report['cleanup_errors']=[]
  held=0
  for cmd in ['INPUT CONTROLLER 0 1 0']+(['PACKAGE stop pxa-quake-sd']if launched else [])+['INPUT KEY HOME']:
   try:req(cmd)
   except Exception as error:report['cleanup_errors'].append(str(error))
  try:pxadb.NormalFsClient(c).remove(mode_path)
  except Exception as error:report['cleanup_errors'].append(str(error))
  save()
