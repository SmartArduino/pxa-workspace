#!/usr/bin/env python3
"""Measure normal Voxel title/new-game, movement, look and mining on pai-touch.

Install an otherwise unmodified production build under an isolated
pxa-voxel-craft-ui-* identity first. Coordinates are intentionally for the
296x240, 160 DPI pai-touch profile; screenshots verify every measured scene.
No save data or persistent system settings are changed by this harness.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / 'pxadb'))
import pxadb


def distribution(values):
    if not values:
        return {'count': 0}
    ordered = sorted(values)
    return dict(count=len(values), mean_us=statistics.mean(values),
                median_us=statistics.median(values),
                p95_us=ordered[math.ceil(.95*len(values))-1],
                min_us=ordered[0], max_us=ordered[-1])


def heap_usage(snapshot):
    heap = snapshot['heap']
    return {kind+suffix: heap[kind+'_total']-heap[kind+field]
            for kind in ('sram', 'psram')
            for suffix, field in (('_used', '_free'), ('_peak_used', '_min'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--app', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--seconds', type=float, default=6)
    args = parser.parse_args()
    if not args.app.startswith('pxa-voxel-craft-ui-'):
        parser.error('use an isolated ui-* identity, never production data')
    if args.repeat < 1 or not 2 <= args.seconds <= 6:
        parser.error('repeat >= 1 and 2 <= seconds <= 6 required (bounded probe)')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(app=args.app, package_sha256=hashlib.sha256(args.package.read_bytes()).hexdigest(),
                  seconds=args.seconds, runs=[], passed=False,
                  workload='normal production title/new-game; default adaptive view; physics, HUD, audio enabled',
                  latency_scope='timestamped injected pointer sample to Guest, present submission and LCD completion; not physical touch-to-photon')
    epoch = time.monotonic()
    with (args.output/'serial.jsonl').open('w', buffering=1) as log, pxadb.PxaDbClient(args.port, 15) as client:
        app_logs = []
        running = monitoring = recording = False

        def record(kind, payload):
            log.write(json.dumps(dict(seconds=time.monotonic()-epoch, kind=kind, payload=payload))+'\n')

        def receive(frame):
            record(frame.kind, frame.payload)
            if ('app='+args.app+' ') in frame.payload:
                app_logs.append(frame.payload)

        client.log_callback = receive
        client.raw_callback = lambda b: record('RAW', b.decode('utf-8', 'replace'))

        def request(command):
            record('COMMAND', command)
            quiet = client.log_subscribed and command.startswith(('MEMORY', 'PERF', 'SCREENSHOT'))
            if quiet:
                client.unsubscribe_logs(timeout=3)
            frames = client.request(command, timeout=30)
            for frame in frames:
                if frame.kind != 'DATA' or command.startswith(('MEMORY', 'PERF')):
                    record(frame.kind, frame.payload)
            if quiet:
                client.subscribe_logs()
            return frames

        def pump(seconds):
            deadline = time.monotonic()+seconds
            while time.monotonic() < deadline:
                client.pump_logs()
                time.sleep(min(.01, max(0, deadline-time.monotonic())))

        def memory(command='MEMORY'):
            rows = {}
            for frame in request(command):
                if frame.kind == 'DATA':
                    props = pxadb.info_properties(frame.payload)
                    key = props.pop('scope')
                    name = props.pop('name', '')
                    rows[key+(':'+name if name else '')] = {k:int(v) for k,v in props.items()}
            return rows

        def pointer(phase, x, y):
            request(f'INPUT POINTER {phase} {x} {y} 0')
            request('INPUT SYNC')

        def tap(x, y):
            pointer('DOWN', x, y); pump(.15); pointer('UP', x, y)

        def screenshot(name):
            for retry in range(6):
                try:
                    shot = pxadb.screenshot_capture(request('SCREENSHOT JPEG'))
                    break
                except pxadb.PxaDbError as error:
                    if retry == 5 or not pxadb.screenshot_retryable(error):
                        raise
                    pump(.2)
            pxadb.write_screenshot(shot, args.output/(name+'.jpg'))
            if shot.metadata.get('source') != 'surface':
                raise RuntimeError('game surface is obscured: '+str(shot.metadata))
            if (int(shot.metadata['width']),int(shot.metadata['height'])) != (296,240):
                raise RuntimeError('this gesture script requires 296x240 pai-touch')
            return shot.metadata

        def input_metrics():
            metrics = {}
            for frame in request('PERF INPUT'):
                if frame.kind in ('DATA','OK') and frame.payload:
                    metrics.update({k:int(v) for k,v in pxadb.info_properties(frame.payload).items()})
            for stage in ('guest','present','visible'):
                count = metrics.get(stage+'_count',0)
                metrics[stage+'_mean_us'] = metrics.get(stage+'_total_us',0)/count if count else None
            return metrics

        def measure(row, name, gesture=None, launch=False):
            nonlocal monitoring, recording
            if not launch:
                request('MEMORY START'); monitoring = True
            before = memory()
            request('PERF START'); recording = True
            input_metrics()  # discard all prior input/history
            begin = time.monotonic()
            log_begin = len(app_logs)
            if gesture:
                gesture(begin)
            pump(max(0, begin+args.seconds-time.monotonic()))
            metrics = input_metrics()
            probe = pxadb.info_properties(request('PERF STOP')[-1].payload); recording = False
            peak = memory('MEMORY STOP'); monitoring = False
            sample = dict(name=name, elapsed_seconds=time.monotonic()-begin,
                          probe=probe, input=metrics, memory_before=before, memory=peak,
                          heap=heap_usage(peak), app_logs=app_logs[log_begin:])
            for kind in ('raster','present'):
                values = []
                for offset in range(0,int(probe[kind]),16):
                    props = pxadb.info_properties(request(f'PERF READ {kind} {offset}')[-1].payload)
                    chunk = [int(v) for v in props['values'].split(',') if v]
                    if int(props['offset']) != offset or len(chunk) != int(props['count']):
                        raise RuntimeError('malformed probe samples')
                    values.extend(chunk)
                if len(values) != int(probe[kind]):
                    raise RuntimeError('incomplete probe capture')
                sample[kind+'_us'] = values
                sample[kind] = distribution(values)
            if any(int(probe[k]) for k in ('raster_overflow','present_overflow','surface_changed')):
                raise RuntimeError('invalid probe window: '+str(probe))
            if not sample['present_us']:
                raise RuntimeError('no distinct LCD-complete frames')
            sample['display_fps'] = 1e6/sample['present']['mean_us']
            sample['screenshot'] = screenshot(f"run-{row['index']}-{name}")
            request('PERF CLEAR')
            row['scenes'].append(sample)
            (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            print(json.dumps(dict(run=row['index'],scene=name,fps=sample['display_fps'],heap=sample['heap'],latency=metrics)),flush=True)

        def walk(begin):
            pointer('DOWN',78,140); pump(.12)
            pointer('MOVE',78,100)
            for step in range(1,6):
                pump(max(0,begin+step*args.seconds/6-time.monotonic()))
                pointer('MOVE',78,100 if step%2 else 175)

        def look(begin):
            pointer('DOWN',185,110)
            for move in range(24):
                pump(max(0,begin+(move+1)*args.seconds/25-time.monotonic()))
                pointer('MOVE',220 if move%2==0 else 185,115 if move%2==0 else 110)

        def mine(begin):
            pointer('DOWN',275,86)
            for step in range(1,6):
                pump(max(0,begin+step*args.seconds/6-time.monotonic()))
                pointer('MOVE',275,86)

        try:
            report['hello'] = client.hello()
            if not all(cap in report['hello'] for cap in ('heap-local','perf-raster')):
                raise RuntimeError('firmware needs local heap and raster probes')
            packages = [f.payload for f in request('PACKAGES') if f.kind=='PKG']
            if any('active=1' in p for p in packages):
                raise RuntimeError('stop the active application before measuring')
            report['packages'] = packages
            client.subscribe_logs()
            # Wake/unlock without selecting a launcher icon.
            request('INPUT KEY HOME'); pump(.3)
            request('INPUT SWIPE 148 216 148 45 400 8'); pump(1)
            request('INPUT KEY HOME'); pump(.5)
            for index in range(args.repeat):
                row = dict(index=index,scenes=[]); report['runs'].append(row)
                request('PERF CLEAR')
                row['memory_before_launch'] = memory()
                request('MEMORY START'); monitoring = True
                launch_time = time.monotonic()
                request(f'PACKAGE run {args.app}'); running = True
                # Audio/session readiness precedes loading all textures/fonts
                # and the first menu. Allow those asynchronous jobs to finish.
                pump(8)
                # Only the first launch may show the audio grant. Wait until
                # it paints; approving it cannot alter system volume settings.
                if not any('Voxel audio ready' in line for line in app_logs):
                    request('INPUT TAP 199 183'); request('INPUT SYNC'); pump(8)
                row['launch_memory'] = memory('MEMORY STOP'); monitoring = False
                row['launch_heap'] = heap_usage(row['launch_memory'])
                row['title'] = screenshot(f'run-{index}-title')
                row['title_ready_upper_bound_seconds'] = time.monotonic()-launch_time
                request('MEMORY START'); monitoring = True
                # New-game timing includes generation and first mesh build.
                measure(row,'new-game',lambda begin: tap(148,96),launch=True)
                request('INPUT CANCEL'); pump(12)
                measure(row,'standing')
                measure(row,'walking',walk); request('INPUT CANCEL'); pump(1)
                measure(row,'turning',look); request('INPUT CANCEL'); pump(1)
                # Look down at nearby terrain through the normal camera path.
                pointer('DOWN',185,100); pump(.12)
                pointer('MOVE',185,205); pump(.2); pointer('UP',185,205); pump(1)
                row['before_mining'] = screenshot(f'run-{index}-before-mining')
                measure(row,'mining',mine); request('INPUT CANCEL'); pump(1)
                tap(254,29); pump(.5)
                row['pause'] = screenshot(f'run-{index}-pause')
                tap(148,72); pump(.5)  # resume
                tap(255,213); pump(.5)
                row['inventory'] = screenshot(f'run-{index}-inventory')
                request(f'PACKAGE stop {args.app}'); running = False
                pump(2); row['memory_after_stop'] = memory()
                app_logs.clear()
                (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            report['passed'] = True
        finally:
            for command,needed in [('INPUT CANCEL',True),('PERF CLEAR',True),
                                   ('MEMORY STOP',monitoring),(f'PACKAGE stop {args.app}',running)]:
                if needed:
                    try: request(command)
                    except Exception as error: report.setdefault('cleanup_errors',[]).append(str(error))
            report['elapsed_seconds'] = time.monotonic()-epoch
            (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if report['passed'] and not report.get('cleanup_errors') else 1


if __name__ == '__main__':
    raise SystemExit(main())
