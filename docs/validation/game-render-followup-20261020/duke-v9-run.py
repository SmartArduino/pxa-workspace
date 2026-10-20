import os,sys,time,json,statistics,hashlib
from pathlib import Path
os.environ['PXADB_BAUD']='2000000';sys.path.insert(0,'tools/pxadb');import pxadb
case=sys.argv[1]; assert case in ('baseline','native')
out=Path('local/duke-v9-'+case);out.mkdir(exist_ok=True)
control=Path('local/duke-v9-control.json')
ready_stamp=0.0; latest_position={}; position_device_stamp=0; launch_marker=0; latest_device_stamp=0
report={'app':'pxa-duke3d-sd','windows':[],'actions':[]}
held=0;last_keep=0;done=False;processing=False
with (out/'serial.jsonl').open('w',buffering=1)as log,pxadb.PxaDbClient('/dev/ttyACM0',10)as c:
 def rec(k,p):
  global ready_stamp,latest_position,position_device_stamp,latest_device_stamp
  log.write(json.dumps({'time':time.monotonic(),'kind':k,'payload':p})+'\n')
  stamp=p.split('\t',1)[0]
  if stamp.isdigit():latest_device_stamp=max(latest_device_stamp,int(stamp))
  if 'PXA DUKE INPUT 'in p and stamp.isdigit() and int(stamp)>max(launch_marker,position_device_stamp):
   position_device_stamp=int(stamp)
   latest_position={k:int(v)for k,v in __import__('re').findall(r'(\w+)=(-?\d+)',p)}
   ready_stamp=time.monotonic() if latest_position.get('mode')==4 else 0.0

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
  if 'tap'in a:
   x,y=a['tap'];buttons=a.get('buttons',0)
   req(f'INPUT POINTER DOWN {x} {y} {buttons}');pump(.25,False)
   req(f'INPUT POINTER UP {x} {y} {buttons}');pump(a.get('settle',1),False)
  elif 'button'in a:
   held=a['button'];req(f'INPUT CONTROLLER 0 1 {held}');pump(a.get('seconds',.5),False)
   held=0;req('INPUT CONTROLLER 0 1 0');pump(1,False)
  elif 'shot'in a:shot(a['shot'])
  elif 'measure'in a:
   name=a['measure'];seconds=a.get('seconds',60);samples=[]
   ready_deadline=time.monotonic()+180
   while time.monotonic()-ready_stamp>=8:
    assert time.monotonic()<ready_deadline,'No fresh actual-game mode=4 record'
    pump(1,False)
   report['position_before']=dict(latest_position)
   before=shot(name+'-before');pump(a.get('warmup',30),False)
   native_before=dict(latest_position)
   start=time.monotonic()
   for i in range(seconds):
    pump(start+i+1-time.monotonic(),False)
    fs=req('PERF FPS');v=int(pxadb.info_properties(fs[-1].payload)['presentation_fps_x10'])/10
    samples.append({'seconds':time.monotonic()-start,'fps':v})
    if i%10==9:print('DUKE FPS',name,i+1,v,flush=True)
   values=[x['fps']for x in samples]
   assert time.monotonic()-ready_stamp<8,'Gameplay gate stale at end'
   report['position_after']=dict(latest_position)
   if case=='native':assert latest_position.get('hb',0)>native_before.get('hb',0)and latest_position.get('hr',-1)==0,latest_position
   row={'label':name,'qualified_actual_gameplay':False,'qualification_note':'Requires visual review of both actual mission screenshots; menu/loading never qualifies.','fps_samples':samples,'fps':{'median':statistics.median(values),'mean':statistics.mean(values),'min':min(values),'max':max(values)},'before':before,'after':shot(name+'-after'),'fresh_actual_game_mode_before_and_after':True,'native_before':native_before,'native_after':dict(latest_position)}
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
  ps=req('PACKAGES');assert not any('active=1'in f.payload for f in ps if f.kind=='PKG')
  report['packages_before']=[f.payload for f in ps if f.kind=='PKG']
  c.subscribe_logs();req('INPUT KEY HOME');pump(.3)
  req('INPUT POINTER DOWN 160 185 0');pump(.15)
  for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.15)
  req('INPUT POINTER UP 160 53 0');pump(.4)
  launch_marker=latest_device_stamp
  req('PACKAGE run pxa-duke3d-sd');report['launched_at']=time.monotonic();save()
  pump(20);shot('launch')
  end=time.monotonic()+900
  while not done and time.monotonic()<end:pump(.5)
 finally:
  held=0;req('INPUT CONTROLLER 0 1 0')
  req('PACKAGE stop pxa-duke3d-sd');pump(1,False);req('INPUT KEY HOME');save()
