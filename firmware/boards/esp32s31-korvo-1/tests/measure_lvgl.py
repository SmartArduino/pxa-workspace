#!/usr/bin/env python3
"""Measure Korvo UI completion events, CPU runtime samples and local heap peaks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/pxadb'))
import pxadb


def require_settings_header(path):
    """Reject the launcher before sending a settings-list drag.

    On this 800x480 profile, Settings has a title below the status bar while
    the launcher leaves that band empty. Package/serial checks are separate;
    this is a page check for the measured profile, not general-purpose OCR.
    """
    from PIL import Image
    with Image.open(path) as picture:
        picture = picture.convert('RGB')
        background = picture.getpixel((400, 50))
        contrasting = sum(
            sum(abs(channel - reference) for channel, reference in zip(pixel, background)) > 150
            for pixel in picture.crop((8, 32, 100, 75)).getdata())
    if contrasting < 60:
        raise RuntimeError('Settings title is absent; verify launcher coordinates before retrying')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/ttyUSB0')
    parser.add_argument('--expected-serial', required=True)
    parser.add_argument('--page', choices=('settings', 'shade'), required=True)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--settings-x', type=int, default=690)
    parser.add_argument('--settings-y', type=int, default=185)
    parser.add_argument('--shade-x', type=int, default=600,
                        help='Use empty shade space, away from edge-back targets')
    parser.add_argument('--packages-reference', type=Path,
                        help='Reject a changed installed-app list before injecting input')
    args = parser.parse_args()
    if (args.output / 'report.json').exists(): parser.error('Use a fresh output directory')
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ['PXADB_BAUD'] = '2000000'
    report = dict(page=args.page, firmware_sha256=hashlib.sha256(args.firmware.read_bytes()).hexdigest(),
                  procedure_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  passed=False, runs=[], errors=[])
    (args.output/'measure_lvgl.py').write_bytes(Path(__file__).read_bytes())
    samples=[]
    with (args.output/'serial.jsonl').open('w',buffering=1) as log, pxadb.PxaDbClient(args.port,15) as client:
        def record(kind,payload):
            log.write(json.dumps(dict(host_time=time.monotonic(),kind=kind,payload=payload))+'\n')
            if any(x in payload for x in ('Guru Meditation','assert failed','Task watchdog got triggered','LCD underrun','BENCHMARK_INVALID')):
                report['errors'].append(payload)
            if 'ACCEL_SAMPLE ' in payload:
                row={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',payload.split('ACCEL_SAMPLE ',1)[1])}
                samples.append(row)
        client.log_callback=lambda f:record(f.kind,f.payload)
        client.raw_callback=lambda b:record('RAW',b.decode('utf8','replace'))
        def request(command):
            record('COMMAND',command)
            frames=client.request(command,timeout=30)
            for f in frames:
                if command != 'SCREENSHOT JPEG' or f.kind!='DATA':record(f.kind,f.payload)
            if command.startswith('INPUT ') and pxadb.info_properties(frames[-1].payload).get('status','processed')!='processed':
                raise RuntimeError('Input was not processed: '+command)
            return frames
        def pump(seconds):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                client.pump_logs();time.sleep(.002)
        def point(phase,x,y):
            return pxadb.info_properties(request(f'INPUT POINTER {phase} {x} {y} 0')[-1].payload)
        def capture(name):
            for retry in range(3):
                try:
                    shot=pxadb.screenshot_capture(request('SCREENSHOT JPEG'));break
                except pxadb.PxaDbError as e:
                    if retry==2 or not pxadb.screenshot_retryable(e):raise
                    pump(.2)
            if int(shot.metadata['width'])!=800 or int(shot.metadata['height'])!=480:
                raise RuntimeError('Expected 800x480 display')
            pxadb.write_screenshot(shot,args.output/(name+'.jpg'))
            return shot.metadata
        def home():
            request('INPUT CANCEL');request('INPUT KEY BACK');request('INPUT KEY BACK')
            request('INPUT KEY HOME');pump(.8)
        def packages():
            return sorted(f.payload for f in request('PACKAGES') if f.kind=='PKG')
        def check_packages():
            current=packages()
            if current!=report['packages']:
                raise RuntimeError('Installed apps changed during measurement; no further gestures')
            return current
        monitoring=recording=control_started=False
        try:
            report['hello']=client.hello()
            identity=pxadb.info_properties(report['hello'])
            if identity.get('target')!='esp32s31' or identity.get('serial')!=args.expected_serial:
                raise RuntimeError('Unexpected board; no input performed')
            report['packages']=packages()
            if args.packages_reference:
                reference=json.loads(args.packages_reference.read_text())
                expected=reference['packages'] if isinstance(reference,dict) else reference
                if report['packages']!=sorted(expected):
                    raise RuntimeError('Installed apps differ from the reference; no input performed')
            client.subscribe_logs()
            control_started=True
            home();report['launcher']=capture('launcher')
            if args.page=='shade':
                # Verify both endpoints outside the timed window. At this
                # position the initial press avoids Wi-Fi, volume, and theme.
                for start_y,end_y,name in ((15,400,'shade-open'),(400,15,'shade-closed')):
                    point('DOWN',args.shade_x,start_y);pump(.08)
                    for step in range(1,31):
                        point('MOVE',args.shade_x,round(start_y+(end_y-start_y)*step/30));pump(.02)
                    point('UP',args.shade_x,end_y);pump(.5)
                    capture(name)
                home();report['launcher']=capture('launcher')
            for run in range(args.runs):
                check_packages()
                home()
                if args.page=='settings':
                    point('DOWN',args.settings_x,args.settings_y);pump(.18)
                    point('UP',args.settings_x,args.settings_y);pump(2)
                row=dict(index=run,before_frame=capture(f'{run}-before'))
                report['runs'].append(row)
                if args.page=='settings':
                    require_settings_header(args.output/f'{run}-before.jpg')
                check_packages()
                request('PERF CLEAR');request('PERF START display');recording=True
                request('MEMORY START');monitoring=True
                row['memory_before']=[f.payload for f in request('MEMORY') if f.kind=='DATA']
                samples.clear()
                first_us=None
                for leg in range(6):
                    if args.page=='settings':
                        start_y,end_y=(400,140) if leg%2==0 else (140,400)
                        x=400
                    else:
                        start_y,end_y=(15,400) if leg%2==0 else (400,15)
                        x=args.shade_x
                    down=point('DOWN',x,start_y)
                    if first_us is None:first_us=int(down['device_us'])
                    # INPUT acknowledgements update an async mailbox. Hold the
                    # edge press until LVGL has sampled it before moving away.
                    pump(.12)
                    start=time.monotonic()
                    for step in range(1,31):
                        pump(max(0,start+step*.02-time.monotonic()))
                        point('MOVE',x,round(start_y+(end_y-start_y)*step/30))
                    up=point('UP',x,end_y);pump(.35)
                row['input_start_us']=first_us;row['input_end_us']=int(up['device_us'])
                row['perf']=pxadb.info_properties(request('PERF STOP')[-1].payload);recording=False
                row['memory_after']=[f.payload for f in request('MEMORY STOP') if f.kind=='DATA'];monitoring=False
                perf=row['perf']
                if int(perf['present_overflow']) or int(perf['surface_changed']):
                    raise RuntimeError('Invalid completion recording')
                if not int(perf['completed']):raise RuntimeError('No completed UI frames')
                row['interval_us']=[]
                for offset in range(0,int(perf['present']),16):
                    data=pxadb.info_properties(request(f'PERF READ present {offset}')[-1].payload)
                    row['interval_us'] += [int(v) for v in data['values'].split(',') if v]
                row['display_fps']=int(perf['completed'])*1e6/int(perf['window_us'])
                row['cpu_samples']=[s for s in samples if s['cpu_valid'] and s['device_us']-s['elapsed_us']>=first_us and s['device_us']<=row['input_end_us']]
                elapsed=sum(s['elapsed_us'] for s in row['cpu_samples'])
                row['cpu_mean']=sum(s['cpu']*s['elapsed_us'] for s in row['cpu_samples'])/elapsed if elapsed else None
                row['accelerator_samples']=list(samples)
                row['after_frame']=capture(f'{run}-after')
                check_packages()
                print(json.dumps(dict(run=run,page=args.page,fps=row['display_fps'],cpu=row['cpu_mean'])),flush=True)
                (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            report['passed']=not report['errors'] and all(r['cpu_mean'] is not None for r in report['runs'])
        except Exception as error:
            report['failure']=str(error)
            raise
        finally:
            cleanup=[]
            if control_started:
                if recording:cleanup.append('PERF STOP')
                if monitoring:cleanup.append('MEMORY STOP')
                cleanup.extend(('INPUT CANCEL','PERF CLEAR'))
            for command in cleanup:
                try:request(command)
                except Exception as error:report.setdefault('cleanup_errors',[]).append(str(error))
            if report['passed']:
                try:home()
                except Exception as error:
                    report['passed']=False
                    report.setdefault('cleanup_errors',[]).append(str(error))
            (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    if not report['passed']:raise SystemExit('Measurement failed; inspect report.json')

if __name__=='__main__':main()
