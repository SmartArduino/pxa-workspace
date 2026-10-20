import os,sys,time,json
from pathlib import Path
sys.path.insert(0,'tools/pxadb');import pxadb
os.environ['PXADB_BAUD']='2000000'
out=Path('local/frame-final-compatibility');out.mkdir(exist_ok=True)
r={'passed':False,'logs':[],'apps':[]}; active=None; latest_stamp=0; q2_since=0; q2_ready=0; held_buttons=0
with pxadb.PxaDbClient('/dev/ttyACM0',10)as c:
 def on_log(f):
  global latest_stamp,q2_ready
  r['logs'].append({'kind':f.kind,'payload':f.payload})
  stamp=f.payload.split('\t',1)[0]
  if stamp.isdigit():
   latest_stamp=max(latest_stamp,int(stamp))
   if int(stamp)>q2_since and 'PXA Q2 INPUT ' in f.payload and 'destination=0 ' in f.payload:
    q2_ready=time.monotonic()
 c.log_callback=on_log
 def req(cmd):return c.request(cmd,timeout=30)
 def pump(sec):
  end=time.monotonic()+sec;keep=0
  while time.monotonic()<end:
   c.pump_logs()
   if time.monotonic()-keep>.8:req(f'INPUT CONTROLLER 0 1 {held_buttons}');keep=time.monotonic()
   time.sleep(.01)
 def shot(name):
  c.unsubscribe_logs(timeout=5)
  try:
   s=pxadb.screenshot_capture(req('SCREENSHOT'));pxadb.write_screenshot(s,out/(name+'.png'));return s.metadata
  finally:c.subscribe_logs()
 try:
  r['hello']=c.hello();assert pxadb.info_properties(r['hello'])['serial']=='1c:29:04:d0:af:5e'
  packages=[f.payload for f in req('PACKAGES') if f.kind=='PKG'];r['packages_before']=packages
  expected={'pxa-jump-jump-3d':'0.1.9','pxa-voxel-craft':'0.12.10','pxa-quake2-sd':'0.1.13','pxa-openlara':'0.5.2','pxa-store':'0.8.8'}
  for app,version in expected.items():
   assert any(p.split('\t')[0].endswith(':'+app)and p.split('\t')[2]==version and 'installed=1'in p for p in packages),(app,packages)
  assert not any('active=1'in p for p in packages)
  c.subscribe_logs();req('INPUT KEY HOME');pump(.3)
  req('INPUT POINTER DOWN 160 185 0');pump(.15)
  for y in [163,139,115,91,67,53]:req(f'INPUT POINTER MOVE 160 {y} 0');pump(.15)
  req('INPUT POINTER UP 160 53 0');pump(.4)
  for app in ['pxa-jump-jump-3d','pxa-voxel-craft']:
   active=app;req('PACKAGE run '+app);pump(6)
   states=[f.payload for f in req('PACKAGES')if f.kind=='PKG']
   assert any(p.split('\t')[0].endswith(':'+app)and'active=1'in p for p in states),(app,states)
   row={'app':app,'version':expected[app],'active':True,'screenshot':shot(app)}
   r['apps'].append(row);print(row,flush=True)
   req('PACKAGE stop '+app);active=None;pump(1)
  active='pxa-quake2-sd';q2_since=latest_stamp;q2_ready=0
  req('PACKAGE run '+active);pump(25);r['q2_launch']=shot('q2-launch')
  def button(bits):
   global held_buttons
   held_buttons=bits
   req(f'INPUT CONTROLLER 0 1 {bits}');pump(.5)
   held_buttons=0
   req('INPUT CONTROLLER 0 1 0');pump(1)
  r['q2_menu']=shot('q2-menu') # Q2 starts with +menu_main; START would close it.
  button(16);r['q2_singleplayer']=shot('q2-singleplayer')
  button(16);pump(30);r['q2_loading']=shot('q2-loading')
  deadline=time.monotonic()+240
  while not q2_ready:
   assert time.monotonic()<deadline,'Q2 actual-game readiness timed out'
   pump(1)
  pump(6);assert time.monotonic()-q2_ready<8
  r['apps'].append({'app':active,'version':expected[active],'actual_gameplay_ready':True,'screenshot':shot('q2-ready')})
  r['q2_memory']=[{'kind':f.kind,'payload':f.payload}for f in req('MEMORY')]
  req('PACKAGE stop '+active);active=None;pump(1)
  req('INPUT KEY HOME');pump(1);r['home']=shot('home')
  r['packages_after']=[f.payload for f in req('PACKAGES')if f.kind=='PKG']
  assert not any('active=1'in p for p in r['packages_after'])
  r['passed']=True
 finally:
  if active:
   req('INPUT CONTROLLER 0 1 0');req('PACKAGE stop '+active);req('INPUT KEY HOME')
  (out/'report.json').write_text(json.dumps(r,indent=2)+'\n')
