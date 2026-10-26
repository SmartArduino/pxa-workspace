from pathlib import Path
import sys,os,time,json,re,statistics,traceback,subprocess,math
ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tools/pxadb'),str(ROOT/'tools/game-perf')]
import pxadb
from usb_keep_awake import usb_parent
from run_main_loop_probe import log_barrier,require_idle_packages
import argparse
ap=argparse.ArgumentParser();ap.add_argument('game',choices=['duke','quake']);ap.add_argument('output',type=Path);ap.add_argument('--elf',required=True);ap.add_argument('--mode',type=int,choices=[1,5,13]);ap.add_argument('--version',default='0.5.0');ap.add_argument('--controls',action='store_true');ap.add_argument('--controls-only',action='store_true');ap.add_argument('--expect-host-mix',action='store_true');ap.add_argument('--expect-light-cache',action='store_true');ap.add_argument('--idle-lock',action='store_true');a=ap.parse_args()
mode_path=None
out=a.output;out.mkdir(parents=True,exist_ok=False)
app='pxa-duke3d' if a.game=='duke' else 'pxa-quake';version='0.2.0' if a.game=='duke' else a.version
port='/dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00'
assert subprocess.run(['fuser',str(Path(port).resolve())],capture_output=True).returncode==1
p=usb_parent(port);assert [(p/x).read_text().strip() for x in ['idVendor','idProduct','serial']]==['1a86','55d3','5CFF043509']
node=f"/dev/bus/usb/{int((p/'busnum').read_text()):03}/{int((p/'devnum').read_text()):03}"
os.environ['PXADB_BAUD']='2000000'
report={'game':a.game,'qualified':False,'log_history_interval_samples':5};records=[];unique=[];seen=set();held=0;connected=1;last_keep=0.;owned=False;perf=False
with open(node,'rb',buffering=0),(out/'serial.jsonl').open('w',buffering=1) as log,pxadb.PxaDbClient(port,10) as c:
 def record(kind,payload):
  row={'time':time.monotonic(),'kind':kind,'payload':payload};records.append(row);log.write(json.dumps(row)+'\n')
  m=re.match(r'^(\d+)\t',payload)
  if kind=='LOG' and m and (payload not in seen):
   seen.add(payload);unique.append(dict(row,log_ms=int(m[1])))
 def req(command):
  record('COMMAND',command);frames=c.request(command,timeout=30)
  for f in frames:
   if f.kind!='DATA' or not command.startswith('SCREENSHOT'):record(f.kind,f.payload)
  return frames
 c.log_callback=lambda f:record(f.kind,f.payload)
 c.raw_callback=lambda raw:record('RAW',raw.decode('utf8','replace'))
 def mark():return log_barrier(req('INPUT SYNC'))
 def fresh(marker):return [r for r in unique if f'[PXA app={app} ' in r['payload'] and 0<((r['log_ms']-marker)&0xffffffff)<0x80000000]
 def levels(marker):return [r for r in fresh(marker) if ('PXA DUKE INPUT' in r['payload'] and re.search(r'\bmode=4\b',r['payload']) if a.game=='duke' else 'PXA QUAKE ready=1' in r['payload'])]
 def pump(seconds):
  global last_keep
  end=time.monotonic()+max(seconds,0)
  while time.monotonic()<end:
   c.pump_logs()
   if time.monotonic()-last_keep>1:
    req(f'INPUT CONTROLLER 0 {connected} {held}');last_keep=time.monotonic()
   time.sleep(min(.005,max(0,end-time.monotonic())))
 def pulse(bits,seconds):
  global held,connected,last_keep
  barrier=mark();connected=1;held=bits
  press=pxadb.info_properties(req(f'INPUT CONTROLLER 0 1 {bits}')[-1].payload);last_keep=time.monotonic()
  pump(seconds);held=0
  release=pxadb.info_properties(req('INPUT CONTROLLER 0 1 0')[-1].payload);last_keep=time.monotonic();pump(.8)
  item={'bits':bits,'requested_seconds':seconds,'press':press,'release':release,'ack_interval_us':int(release['device_us'])-int(press['device_us']),'log_barrier':barrier}
  report.setdefault('pulses',[]).append(item);return item
 def shot(name,visible=None):
  c.unsubscribe_logs(timeout=5)
  try:
   for i in range(5):
    try:s=pxadb.screenshot_capture(req('SCREENSHOT'));break
    except pxadb.PxaDbError as e:
     if i==4 or not pxadb.screenshot_retryable(e):raise
     time.sleep(.25)
   pxadb.write_screenshot(s,out/(name+'.png'));report.setdefault('screenshots',{})[name]=s.metadata
   if visible is not None:
    assert s.metadata['format']=='rgb565le' and int(s.metadata['width'])==320 and int(s.metadata['height'])==240
    border=[(x,y) for y in (192,239) for x in range(268,320)]+[(x,y) for x in (268,319) for y in range(193,239)]
    white=sum(s.data[y*640+x*2:y*640+x*2+2]==b'\xff\xff' for x,y in border)
    report.setdefault('overlay',{})[name]={'white':white,'total':len(border),'expected_visible':visible}
    assert (white==len(border))==visible,(name,white)
   print('SCREEN',name,flush=True)
  finally:c.subscribe_logs()
 def waitlevel(marker,seconds=30):
  until=time.monotonic()+seconds; refresh=time.monotonic()+6
  while time.monotonic()<until:
   pump(.2)
   if levels(marker):return levels(marker)[-1]
   if time.monotonic()>=refresh:
    # The four-slot log transport can drop a burst. Recover retained history;
    # fresh() still requires an original device stamp newer than the barrier.
    c.unsubscribe_logs(timeout=5);c.subscribe_logs();refresh=time.monotonic()+6
  raise RuntimeError('No fresh actual-level record after marker')
 def position(row):return {k:int(re.search(r'\b'+k+r'=(-?\d+)',row['payload'])[1]) for k in ['x','y','angle']}
 try:
  report['hello']=c.hello();assert pxadb.info_properties(report['hello'])['serial']=='1c:29:04:d0:af:5e'
  report['info']=pxadb.info_properties(req('INFO')[-1].payload);assert report['info']['elf_sha256']==a.elf
  packages=req('PACKAGES');require_idle_packages(packages)
  assert any(f.kind=='PKG' and f':{app}\t' in f.payload and f'\t{version}\t' in f.payload for f in packages),'Wrong installed game version'
  pump(10);require_idle_packages(req('PACKAGES'))
  report['idle_memory']=[{'kind':f.kind,'payload':f.payload} for f in req('MEMORY')]
  mark();owned=True
  if a.mode is not None:
   identity=next(f.payload.split('\t',1)[0] for f in packages if f.kind=='PKG' and f':{app}\t' in f.payload)
   paths={f.payload.split('\t',1)[0]:f.payload.split('\t',1)[1] for f in req('PACKAGE paths '+identity) if f.kind=='PATH'}
   assert 'media' in paths,'No selected media volume'
   mode_path=paths['media']+'/pxa-render-probe-mode'
   fs=pxadb.NormalFsClient(c)
   existing=fs.list(mode_path.rsplit('/',1)[0])
   if any(e[2]=='pxa-render-probe-mode' for e in existing):fs.remove(mode_path)
   mode_file=out/'mode-byte';mode_file.write_text(format(a.mode,'x'));fs.put(mode_file,mode_path)
   report['render_mode']=a.mode
  c.subscribe_logs();req('INPUT KEY HOME');pump(.3)
  req('INPUT POINTER DOWN 160 185 0');pump(.1)
  for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.1)
  req('INPUT POINTER UP 160 53 0');pump(.3)
  launch=mark();report['launch_log_ms']=launch;req(f'PACKAGE run {app}');pump(25);shot('launch')
  if a.game=='duke':
   for i in range(8):
    if levels(launch) and time.monotonic()-levels(launch)[-1]['time']<8:break
    pulse(128,1.2);pump(2);shot('menu-'+str(i))
   waitlevel(launch,30)
  else:
   pulse(64,.4);pump(1);shot('menu')
   pulse(16,.4);pump(1);shot('singleplayer')
   pulse(16,.4);pump(18);shot('newgame')
   waitlevel(launch,30)
  report['pcm_negotiation']=any('PXA PCM source=11025Hz/2ch S16LE sink=48000Hz/1ch' in r['payload'] for r in fresh(launch))
  assert report['pcm_negotiation'],'Source PCM negotiation not observed'
  report['host_mix_observed']=any('PXA AUDIO Host S8 accumulation active' in r['payload'] for r in fresh(launch))
  assert report['host_mix_observed']==a.expect_host_mix,'Unexpected Host audio mix path'
  if not a.controls_only:
   pump(6);shot('level-before-window',False);window=mark();req('PERF START display');perf=True
   started=time.monotonic();report['window_log_ms']=window;report['window_start']=started;samples=[]
   for i in range(60):
    pump(started+i+1-time.monotonic());v=pxadb.info_properties(req('PERF FPS')[-1].payload);samples.append(float(v['presentation_fps_x10'])/10)
    if i%5==4:req('LOGSUB') # Recover dropped live log frames with original timestamps.
    if i%10==9:print('FPS',i+1,samples[-1],flush=True)
   report['window_end']=time.monotonic();report['display']=pxadb.info_properties(req('PERF STOP')[-1].payload);perf=False;req('PERF CLEAR')
   freshlevel=[r for r in levels(window) if started<=r['time']<=report['window_end']]
   assert len(freshlevel)>=8 and ((freshlevel[-1]['log_ms']-freshlevel[0]['log_ms'])&0xffffffff)>=45000,'Insufficient complete level window'
   if a.game=='quake':
    assert all('xyz=544,288,50' in r['payload'] and 'hp=100' in r['payload'] for r in freshlevel),'Scene changed'
   if a.mode is not None:
    assert all(re.search(r'\bmode='+str(a.mode)+r'\b',r['payload']) for r in freshlevel),'Mode changed'
   report['level_records']=freshlevel;report['fps_samples']=samples;report['fps']={'mean':statistics.mean(samples),'median':statistics.median(samples),'min':min(samples),'max':max(samples),'lcd_completed':int(report['display']['completed'])*1e6/int(report['display']['window_us'])}
   report['memory']=[{'kind':f.kind,'payload':f.payload} for f in req('MEMORY')];shot('level-after-window',False)
  if a.controls:
   for ms in [150,100,50]:
    req('INPUT POINTER DOWN 292 212 0');pump(.2);req('INPUT POINTER MOVE 285 207 0');pump(.2);req('INPUT POINTER UP 285 207 0');pump(2);shot(f'touch-{ms}',True)
    p=pulse(16,ms/1000);pump(1);shot(f'pad-{ms}',False)
    edges=[r for r in fresh(p['log_barrier']) if 'PXA PAD controller=0' in r['payload']]
    states=[int(re.search(r'buttons=(\d+)',r['payload'])[1]) for r in edges]
    p['guest_edges']=edges
    assert 16 in states and 0 in states[states.index(16)+1:],('Missing Guest press/release',ms,states)
   shot('release-hidden',False);connected=0;req('INPUT CONTROLLER 0 0 0');pump(1);shot('disconnect',True)
   if a.game=='duke':
    movements=[]
    for bits in [1,2]:
     before=position(waitlevel(mark()));p=pulse(bits,1.0);pump(5);after=position(waitlevel(mark()))
     dx,dy=after['x']-before['x'],after['y']-before['y'];angle=before['angle']*2*math.pi/2048;projection=dx*math.cos(angle)+dy*math.sin(angle)
     m={'bits':bits,'before':before,'after':after,'dx':dx,'dy':dy,'projection':projection};movements.append(m);report['movement']=movements
     assert dx*dx+dy*dy>64*64 and (projection>0 if bits==1 else projection<0),('Wrong movement',m)
     edges=[r for r in fresh(p['log_barrier']) if 'PXA PAD controller=0' in r['payload']];p['guest_edges']=edges
     assert any(f'buttons={bits}' in r['payload'] for r in edges),'Movement edge not observed'
     shot(f'move-{bits}',False)
  if a.controls and a.game=='quake':
   def quake_position(row):
    return tuple(map(int,re.search(r'\bxyz=(-?\d+),(-?\d+),(-?\d+)',row['payload']).groups()))
   movements=[]
   for bits in [1,2]:
    before=quake_position(waitlevel(mark()));p=pulse(bits,.6);pump(3);after=quake_position(waitlevel(mark()))
    delta=[after[i]-before[i] for i in range(3)]
    movements.append({'bits':bits,'before':before,'after':after,'delta':delta});report['movement']=movements
    assert sum(v*v for v in delta[:2])>4,('No actual Quake movement',movements[-1])
    edges=[r for r in fresh(p['log_barrier']) if 'PXA PAD controller=0' in r['payload']];p['guest_edges']=edges
    assert any(f'buttons={bits}' in r['payload'] for r in edges),'Movement edge not observed'
    shot(f'move-{bits}',False)
   assert sum(x*y for x,y in zip(movements[0]['delta'][:2],movements[1]['delta'][:2]))<0,'Forward/backward moved in the same direction'
  if a.idle_lock:
   held=0;connected=0;req('INPUT CANCEL');req('INPUT CONTROLLER 0 0 0')
   # The qualified board persists a 120-second idle lock. PING keeps the
   # debugger responsive without generating input activity during the wait.
   until=time.monotonic()+130;last_ping=0.
   while time.monotonic()<until:
    c.pump_logs()
    if time.monotonic()-last_ping>5:req('PING');last_ping=time.monotonic()
    time.sleep(.01)
   shot('idle-locked')
   resume=mark();req('INPUT SWIPE 160 210 160 55 400 10');pump(3)
   row=waitlevel(resume);shot('idle-unlocked')
   info=pxadb.info_properties(req('INFO')[-1].payload)
   assert info['elf_sha256']==a.elf and info['serial']=='1c:29:04:d0:af:5e'
   report['lock_resume']={'idle_seconds':130,'record':row,'info':info}
  if a.game=='quake':
   cache_marker=report.get('window_log_ms',launch)
   cache_rows=[r for r in fresh(cache_marker) if 'PXA QLIGHT hit=' in r['payload']]
   report['light_cache_records']=cache_rows
   def count(name):
    return sum(int(re.search(r'\b'+name+r'=(\d+)',r['payload'])[1]) for r in cache_rows)
   report['light_cache']={name:count(name) for name in ['hit','miss','bypass','verified','unused_tail']}
   expect_cache=a.expect_light_cache or (a.mode is not None and bool(a.mode & 4))
   assert (report['light_cache']['hit']>0)==expect_cache,'Unexpected cache activity'
   if a.mode is not None and a.mode & 8:
    assert report['light_cache']['verified']>0,'No original-engine comparisons observed'
   assert not any('QLIGHT mismatch' in r['payload'] or 'Cached lighting differs' in r['payload'] for r in fresh(launch)),'Light cache mismatch'
  report['qualified']=True
 except BaseException as e:
  report['error']=repr(e);report['traceback']=traceback.format_exc();raise
 finally:
  if owned:
   for command in ['INPUT CANCEL']+(['PERF STOP','PERF CLEAR'] if perf else []):
    try:req(command)
    except Exception as e:report.setdefault('cleanup_errors',[]).append({'command':command,'error':repr(e)});report['qualified']=False
   stopped=False
   for attempt in range(12):
    try:
     packages=req('PACKAGES')
     if not any(f.kind=='PKG' and f':{app}\t' in f.payload and 'active=1' in f.payload for f in packages):stopped=True;break
     req(f'PACKAGE stop {app}')
    except Exception as e:report.setdefault('stop_retries',[]).append({'attempt':attempt,'error':repr(e)})
    time.sleep(.5)
   report['stopped']=stopped
   if not stopped:report['qualified']=False
   else:
    pump(1)
    report['after_stop_memory']=[{'kind':f.kind,'payload':f.payload} for f in req('MEMORY')]
    req('INPUT KEY HOME')
    if mode_path is not None:
     try:pxadb.NormalFsClient(c).remove(mode_path)
     except Exception as e:report.setdefault('cleanup_errors',[]).append({'command':'remove owned probe mode','error':repr(e)});report['qualified']=False
  (out/'measurement.json').write_text(json.dumps(report,indent=2)+'\n')
  print(json.dumps({k:report[k] for k in ['qualified','fps','error','movement','stopped'] if k in report},indent=2),flush=True)
