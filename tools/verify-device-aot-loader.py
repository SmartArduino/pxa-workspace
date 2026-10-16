#!/usr/bin/env python3
"""Exercise multiple installed apps on a physical board; keep raw evidence.

Checks launch/stop, HOME/resume, input and repeated starts. The active-state
latency includes command/polling overhead and is not optical display latency.
Heap windows are stopped before screenshots allocate their temporary buffers.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / 'pxadb'))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--baud', type=int, default=2000000)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--grant-media', action='store_true',
                        help='Allow the known 296x240 media permission dialog for Jump Jump 3D')
    parser.add_argument('--apps', nargs='+', default=[
        'pxa-cpp-gestures', 'pxa-loader-wasm', 'pxa-novel-reader', 'pxa-jump-jump-3d'])
    parser.add_argument('--reject-apps', nargs='*', default=[],
                        help='Installed, deliberately invalid QA packages that must fail to load')
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('rounds must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ['PXADB_BAUD'] = str(args.baud)
    report = dict(port=args.port, baud=args.baud, passed=False, runs=[], errors=[],
                  rejected=[], expected_rejections=[])
    started = time.monotonic()

    def save():
        (args.output/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')

    with (args.output/'serial.jsonl').open('w', buffering=1) as log, \
            pxadb.PxaDbClient(args.port, timeout=30) as client:
        def record(kind, payload):
            entry = dict(seconds=time.monotonic()-started, kind=kind, payload=payload)
            log.write(json.dumps(entry, ensure_ascii=False)+'\n')
            if isinstance(payload,str) and any(word in payload for word in (
                    'Package launch failed', 'load failed:')):
                for app in args.reject_apps:
                    if f':{app}' in payload or f'~{app}/' in payload:
                        report['expected_rejections'].append(dict(entry,app=app))
                        return
            if isinstance(payload, str) and any(word in payload for word in (
                'Guru Meditation', 'assert failed', "panic'ed", 'Task watchdog',
                'PXA Guest exception', 'Package launch failed', 'rst:0x')):
                report['errors'].append(entry)

        client.log_callback = lambda frame: record(frame.kind, frame.payload)
        client.raw_callback = lambda data: record('RAW', data.decode('utf-8', 'replace'))

        def command(text, timeout=30):
            record('COMMAND', text)
            frames = client.request(text, timeout=timeout)
            rows = [dict(kind=f.kind, payload=f.payload) for f in frames if f.kind != 'DATA' or text != 'SCREENSHOT']
            record('RESPONSE', rows)
            return frames

        def pump(seconds):
            end=time.monotonic()+seconds
            while time.monotonic()<end:
                client.pump_logs();time.sleep(.01)

        def wait_active(app, active):
            end=time.monotonic()+25
            while time.monotonic()<end:
                frames=command('PACKAGES')
                rows=[f.payload for f in frames if f.kind=='PKG' and f':{app}\t' in f.payload]
                if rows and all(f'active={int(active)}' in r.split('\t')[-1].split(';') for r in rows):
                    return
                pump(.15)
            raise RuntimeError(f'{app}: active={int(active)} was not reached')

        def memory():
            return [dict(kind=f.kind,payload=f.payload) for f in command('MEMORY')]

        def screenshot(name):
            for retry in range(3):
                try:
                    shot=pxadb.screenshot_capture(command('SCREENSHOT'))
                    pxadb.write_screenshot(shot,args.output/name)
                    return shot.metadata
                except pxadb.PxaDbError as error:
                    if retry==2 or not pxadb.screenshot_retryable(error):raise
                    pump(.2)

        report['hello']=client.hello();client.subscribe_logs()
        report['before']=memory()
        for turn in range(args.rounds):
            for app in args.apps:
                row=dict(app=app,round=turn+1,passed=False)
                report['runs'].append(row);save()
                try:
                    row['before']=memory();command('MEMORY START')
                    begin=time.monotonic();command(f'PACKAGE run {app}');wait_active(app,True)
                    row['active_ms']=round((time.monotonic()-begin)*1000,1)
                    pump(1)
                    if args.grant_media and app=='pxa-jump-jump-3d':
                        command('INPUT TOUCH DOWN 200 185 0');pump(.1)
                        command('INPUT TOUCH UP 200 185 0');pump(1)
                    row['launch_window']=[dict(kind=f.kind,payload=f.payload) for f in command('MEMORY STOP')]
                    row['running']=memory()
                    if turn==0:row['screenshot']=screenshot(f'{app}-initial.png')
                    command('INPUT TOUCH DOWN 80 148 0');pump(.65)
                    command('INPUT TOUCH UP 80 148 0');pump(.2)
                    if turn==0:screenshot(f'{app}-input.png')
                    command('INPUT KEY HOME');pump(.4)
                    command(f'PACKAGE run {app}');wait_active(app,True);pump(.4)
                    row['after_resume']=memory()
                    command(f'PACKAGE stop {app}');wait_active(app,False);pump(1)
                    row['after']=memory();row['passed']=True
                except Exception as error:
                    row['error']=str(error)
                    for text in ('MEMORY STOP', f'PACKAGE stop {app}'):
                        try:command(text)
                        except Exception:pass
                save()
        for turn in range(args.rounds):
            for app in args.reject_apps:
                row=dict(app=app,round=turn+1,passed=False)
                report['rejected'].append(row)
                try:
                    row['before']=memory()
                    def rejection_count():
                        return sum(e['app']==app for e in report['expected_rejections'])
                    mark=rejection_count()
                    command(f'PACKAGE run {app}')
                    end=time.monotonic()+25
                    while rejection_count()==mark:
                        if time.monotonic()>end:raise RuntimeError('missing explicit load rejection')
                        pump(.1)
                    wait_active(app,False);pump(.3)
                    row['after']=memory()
                    # A healthy control app must still start and stop after
                    # every rejected load; a responding serial task is insufficient.
                    control=args.apps[0]
                    command(f'PACKAGE run {control}');wait_active(control,True);pump(.4)
                    command(f'PACKAGE stop {control}');wait_active(control,False);pump(.4)
                    row['passed']=True
                except Exception as error:
                    row['error']=str(error)
                save()
        pump(1)
        report['after']=memory()
        report['passed']=all(r['passed'] for r in report['runs']+report['rejected']) and not report['errors']
        save()
        print(json.dumps(dict(passed=report['passed'],runs=len(report['runs']),errors=len(report['errors']))))
    return 0 if report['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
