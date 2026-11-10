#!/usr/bin/env python3
"""Measure completed panel frames of the P4 display probe at all four angles.

Use the same signed native package before and after a firmware change. Captures
and memory walks run outside timing windows. Only an explicitly installed probe
can be removed; other packages and private data are never cleared.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent/'pxadb'))
import pxadb

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',required=True)
p.add_argument('--baud',type=int,default=2000000)
p.add_argument('--package',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--repeat',type=int,default=3)
p.add_argument('--seconds',type=float,default=4)
p.add_argument('--install',action='store_true')
p.add_argument('--keep-installed',action='store_true')
p.add_argument('--cleanup',action='store_true')
p.add_argument('--ownership',type=Path,required=True)
p.add_argument('--expected-elf',help='reject a device running a different firmware ELF SHA-256')
a=p.parse_args()
if a.repeat<1 or not 1<=a.seconds<=4:p.error('repeat >= 1 and seconds in [1,4] required')
if a.install and a.cleanup:p.error('install and cleanup are separate phases')
if (a.output/'serial.jsonl').exists():p.error('choose a new output directory to preserve earlier measurements')
a.output.mkdir(parents=True,exist_ok=True)
os.environ['PXADB_BAUD']=str(a.baud)
identity=pxadb.package_manifest_identity(a.package)
if identity!='pxa-p4-display-probe':p.error('expected the dedicated display-probe package')
with a.package.open('rb')as package_file:
 digest=hashlib.file_digest(package_file,'sha256').hexdigest()
report=dict(passed=False,package_sha256=digest,runs=[])
events=[]
def require(condition,message):
 if not condition:raise RuntimeError(message)

with (a.output/'serial.jsonl').open('w',buffering=1)as log,pxadb.open_client(a.port,15)as c:
 def record(kind,payload):
  events.append(payload);log.write(json.dumps(dict(at=time.monotonic(),kind=kind,payload=payload))+'\n')
 c.log_callback=lambda f:record(f.kind,f.payload)
 c.raw_callback=lambda raw:record('RAW',raw.decode(errors='replace'))
 def request(cmd):
  record('COMMAND',cmd);frames=c.request(cmd,timeout=15)
  for f in frames:
   if f.kind!='DATA'or not cmd.startswith('SCREENSHOT'):record(f.kind,f.payload)
  return frames
 def properties(cmd):
  result={}
  for f in request(cmd):
   if f.kind in ['DATA','OK']:result.update(pxadb.info_properties(f.payload))
  return result
 def packages():return [f.payload for f in request('PACKAGES')if f.kind=='PKG']
 def pump(seconds):
  end=time.monotonic()+seconds
  while time.monotonic()<end:c.pump_logs();time.sleep(.01)
 def until(marker,start):
  end=time.monotonic()+20
  while not any(marker in text for text in events[start:]):
   if time.monotonic()>end:raise RuntimeError('missing '+marker)
   c.pump_logs();time.sleep(.02)
 info=properties('INFO');require(info.get('board')=='espp4-a','expected an ESPP4-A device')
 require(info.get('elf_sha256'),'firmware does not report its ELF SHA-256')
 if a.expected_elf:require(info['elf_sha256']==a.expected_elf,'device firmware differs from --expected-elf')
 report['info']=info
 before=packages();require(not any('active=1'in row for row in before),'stop the current app before measuring')
 if a.install:
  require(not any(row.split('\t')[0].split(':')[-1]==identity for row in before),'probe already installed; refusing to overwrite it')
  args=pxadb.build_parser().parse_args(['package','install',str(a.package.resolve()),'--port',a.port,'--timeout','60','--volume','internal','--via','serial'])
  pxadb.command_package_install(args,connection=c,quiet=True)
  a.ownership.write_text(json.dumps(dict(package_sha256=digest,original=before),indent=2)+'\n')
 else:
  lease=json.loads(a.ownership.read_text());require(lease['package_sha256']==digest,'probe differs from ownership record')
  require(any(row.split('\t')[0].split(':')[-1]==identity for row in before),'owned probe is not installed')
 c.subscribe_logs();running=False
 try:
  request('INPUT SWIPE 240 760 240 160 300 12')
  started=len(events);request('PACKAGE run '+identity);running=True
  for rotation in range(4):
   until(f'P4 probe ready rotation={rotation}',started);pump(1)
   for repeat in range(a.repeat):
    request('PERF START');pump(a.seconds)
    counts={k:int(v)for k,v in properties('PERF STOP').items()}
    require(counts['present'] and counts.get('completed',1)>0,'no completed display frames: '+str(counts))
    require(not any(counts.get(k,0)for k in ['raster_overflow','present_overflow','surface_changed']),'invalid measurement window: '+str(counts))
    row=dict(rotation=rotation,repeat=repeat,counters=counts)
    for kind in ['raster','present']:
     values=[]
     for offset in range(0,counts[kind],16):
      page=properties(f'PERF READ {kind} {offset}')
      values.extend(int(v)for v in page['values'].split(',')if v)
     require(len(values)==counts[kind],'incomplete '+kind+' samples')
     row[kind+'_us']=values
    row['display_fps']=1e6/statistics.mean(row['present_us'])
    row['raster_mean_us']=statistics.mean(row['raster_us'])if row['raster_us']else 0
    row['memory']=[f.payload for f in request('MEMORY')if f.kind=='DATA']
    report['runs'].append(row)
    (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('ROTATION',rotation,'SAMPLE',repeat,'FPS',round(row['display_fps'],2),'raster_us',round(row['raster_mean_us']),flush=True)
   c.unsubscribe_logs()
   for attempt in range(6):
    try:
     shot=pxadb.screenshot_capture(request('SCREENSHOT JPEG'));break
    except pxadb.PxaDbError as error:
     if attempt==5 or not pxadb.screenshot_retryable(error):raise
     pump(.3)
   pxadb.write_screenshot(shot,a.output/f'rotation-{rotation}.jpg')
   c.subscribe_logs()
   if rotation<3:
    started=len(events);request(f"INPUT TAP {int(shot.metadata['width'])//2} {int(shot.metadata['height'])//2}");request('INPUT SYNC')
  require(not any(marker in s for s in events for marker in ['Guru Meditation','Guest exception','assert failed','Hardware rotation selftest FAIL']),'device reported a runtime failure')
  report['info_after']=properties('INFO')
  require(report['info_after'].get('elf_sha256')==info['elf_sha256'],'device firmware changed during the measurement')
  report['passed']=True
 except BaseException as error:
  report['error']=str(error)
  raise
 finally:
  try:
   request('PERF CLEAR')
   if running:request('PACKAGE stop '+identity);pump(.5)
   request('INPUT CANCEL');request('INPUT KEY HOME');pump(.3)
   if a.cleanup or (a.install and not a.keep_installed):
    lease=json.loads(a.ownership.read_text());require(lease['package_sha256']==digest,'probe differs from ownership record; refusing to uninstall')
    request('PACKAGE uninstall '+identity);pump(.3)
    after=packages()
    require({tuple(s.split('\t')[:3])for s in after}=={tuple(s.split('\t')[:3])for s in lease['original']},'original app identities or versions changed')
    report['owned_probe_removed']=True
   report['packages_after']=packages()
  except Exception as error:
   report['passed']=False;report['cleanup_error']=str(error)
   if 'error'not in report:raise
  finally:
   (a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
   pxadb.end_control_session(c)
