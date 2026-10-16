#!/usr/bin/env python3
"""Board-specific display-completion and local heap-window benchmark."""
import argparse
import json
import os
from pathlib import Path
import sys
import time

workspace = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(workspace / 'tools/pxadb'))
import pxadb

parser = argparse.ArgumentParser(description='Measure completed Mosaico UI frames with repeatable 480x480 gestures. The test returns Home and opens the selected page.')
parser.add_argument('label')
parser.add_argument('--page', choices=('settings', 'shade'), default='settings')
parser.add_argument('--runs', type=int, default=2)
parser.add_argument('--port', required=True)
parser.add_argument('--baud', type=int, default=2000000)
parser.add_argument('--pointer-id', type=int, choices=(0, 1), default=0,
                    help='Use primary 0 normally; 1 reproduces old firmware whose primary queued taps were broken.')
parser.add_argument('--output', type=Path, default=workspace/'local/mosaico-ui-measurement')
args = parser.parse_args()
if args.runs < 1 or args.baud < 1: parser.error('runs and baud must be positive')
os.environ['PXADB_BAUD'] = str(args.baud)
root = args.output
root.mkdir(parents=True, exist_ok=True)
logs, events, runs = [], [], []
with pxadb.PxaDbClient(args.port, 15) as client:
    hello = client.hello()
    packages = [frame.payload for frame in client.request('PACKAGES') if frame.kind == 'PKG']
    def raw(data):
        line = data.decode(errors='replace').strip()
        if not line: return
        logs.append(line)
        if any(tag in line for tag in ('frame_stats', 'ui_transfer', 'health reset=')):
            print(line, flush=True)
    client.raw_callback = raw
    client.log_callback = lambda frame: logs.append(frame.payload)
    # Read raw ESP console output during requests as well as between them.
    # Do not enable firmware LOGSUB's bounded four-record burst queue.
    client.log_subscribed = True
    def request(command):
        started = time.monotonic()
        frames = client.request(command, timeout=40)
        events.append(dict(command=command, start=started, end=time.monotonic(), response=frames[-1].payload))
        if command.startswith('INPUT ') and pxadb.info_properties(frames[-1].payload).get('status', 'processed') != 'processed':
            raise pxadb.PxaDbError('Input was interrupted or rejected: '+command)
        return frames
    def pump(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            client.pump_logs()
            time.sleep(.002)
    def point(phase, x, y):
        request(f'INPUT POINTER {phase} {x} {y} {args.pointer_id}')
    def capture(name):
        # Serialize diagnostics with bulk DATA frames during the screenshot.
        # Raw ESP console fragments can otherwise split a base64 line.
        client.log_subscribed = False
        client.subscribe_logs()
        try:
            for attempt in range(3):
                try:
                    captured = pxadb.screenshot_capture(request('SCREENSHOT'))
                    break
                except pxadb.PxaDbError as error:
                    if attempt == 2 or not (pxadb.screenshot_retryable(error) or 'screenshot_failed' in str(error)):
                        raise
            pxadb.write_screenshot(captured, root/f'{args.label}-{name}.png')
            return captured.metadata
        finally:
            client.unsubscribe_logs()
            client.log_subscribed = True
    def prepare_page(name):
        # HOME alone does not dismiss the reference UI's notification overlay.
        # Dismiss overlays first; a top-edge wake tap can reveal the shade.
        request('INPUT CANCEL')
        request('INPUT KEY BACK'); pump(.3)
        request('INPUT KEY BACK'); pump(.3)
        request('INPUT KEY HOME'); pump(2)
        capture(name)
        if args.page == 'settings':
            # Fixed installed layout: Settings is the first launcher icon.
            point('DOWN', 90, 140); pump(.08); point('UP', 90, 140); pump(2)
    prepare_page('launcher')
    capture('start')
    memory_running = perf_running = False
    try:
        for run in range(args.runs):
            if run: prepare_page(f'run-{run}-launcher')
            row = {'run': run}
            runs.append(row)
            row['before_frame'] = capture(f'run-{run}-before')
            request('PERF CLEAR')
            request('PERF START display'); perf_running = True
            request('MEMORY START'); memory_running = True
            row['memory_before'] = [f.payload for f in request('MEMORY')]
            if args.page == 'settings':
                point('DOWN', 350, 405)
                row['down_device_us'] = int(pxadb.info_properties(events[-1]['response'])['device_us'])
                y = 405
                for leg in range(10):
                    target = 155 if leg % 2 == 0 else 405
                    start = time.monotonic()
                    for step in range(1, 66):
                        pump(max(0, start + step*.01-time.monotonic()))
                        point('MOVE', 350, round(y + (target-y)*step/65))
                    y = target
                point('UP', 350, y)
            else:
                # Repeated notification-shade reveal/dismiss through the top
                # edge, away from level sliders and quick-toggle buttons.
                for leg in range(8):
                    y, target = (18, 410) if leg % 2 == 0 else (410, 18)
                    point('DOWN', 240, y)
                    if leg == 0: row['down_device_us'] = int(pxadb.info_properties(events[-1]['response'])['device_us'])
                    start = time.monotonic()
                    for step in range(1, 66):
                        pump(max(0, start + step*.01-time.monotonic()))
                        point('MOVE', 240, round(y+(target-y)*step/65))
                    point('UP', 240, target)
                    pump(.15)
            row['perf'] = pxadb.info_properties(request('PERF STOP')[-1].payload); perf_running = False
            row['memory_after'] = [f.payload for f in request('MEMORY STOP')]; memory_running = False
            row['after_frame'] = capture(f'run-{run}-after')
            row['interval_us'] = []
            for offset in range(0, int(row['perf']['present']), 16):
                data = pxadb.info_properties(request(f'PERF READ present {offset}')[-1].payload)
                row['interval_us'] += [int(v) for v in data['values'].split(',') if v]
            frames, window = int(row['perf']['completed']), int(row['perf']['window_us'])
            row['display_fps'] = frames*1e6/window if frames else None
            if not frames:
                raise pxadb.PxaDbError('No completed frames: verify the page screenshots and unlock the display before measuring.')
            row['scanout_count_delta'] = int(row['after_frame']['frame_id'])-int(row['before_frame']['frame_id'])
            row['scanout_window_us'] = int(row['after_frame']['device_us'])-row['down_device_us']
            row['scanout_fps'] = row['scanout_count_delta']*1e6/row['scanout_window_us']
            print(json.dumps({'run': run, 'fps': row['display_fps'], 'scanout_fps': row['scanout_fps'], 'perf': row['perf']}), flush=True)
            request('PERF CLEAR')
            pump(.3)
        capture('end')
    finally:
        cleanup = ([ 'PERF STOP' ] if perf_running else []) + ([ 'MEMORY STOP' ] if memory_running else []) + ['INPUT CANCEL', 'PERF CLEAR']
        for command in cleanup:
            try: request(command)
            except Exception as error: logs.append(f'Cleanup {command}: {error}')
        client.log_subscribed = False
        (root/f'{args.label}.log').write_text('\n'.join(logs)+'\n')
        (root/f'{args.label}.json').write_text(json.dumps(dict(hello=hello,packages=packages,page=args.page,pointer_id=args.pointer_id,runs=runs,events=events),indent=2)+'\n')
