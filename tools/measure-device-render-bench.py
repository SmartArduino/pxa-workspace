#!/usr/bin/env python3
"""Capture real displayed/rendered FPS from the fixed 11-group render benchmark.

Install the package first. A run resets local heap/performance windows; application
submission counters are never used as displayed FPS. Logs include stable and
this-run peak memory, raster telemetry, and the final screen outside that window.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / 'pxadb'))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--app', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeat', type=int, default=1)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('repeat must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(app=args.app, package_sha256=hashlib.sha256(args.package.read_bytes()).hexdigest(),
                  runs=[], passed=False)
    pattern = re.compile(r'BENCH (\d+) ([A-Z0-9-]{5}) V(\d+) R(\d+) MS(\d+)')
    with (args.output / 'serial.jsonl').open('w', buffering=1) as log:
        with pxadb.PxaDbClient(args.port, 15) as client:
            seen, samples = set(), []
            accepting = False

            def record(frame):
                nonlocal accepting
                log.write(json.dumps({'kind': frame.kind, 'payload': frame.payload}) + '\n')
                if frame.payload in seen:
                    return
                seen.add(frame.payload)
                if 'GameRender benchmark start:' in frame.payload:
                    accepting = True
                match = pattern.search(frame.payload)
                if accepting and match:
                    index, group, visible, rendered, raster = match.groups()
                    samples.append(dict(index=int(index), group=group, visible_fps=int(visible),
                                        rendered_fps=int(rendered), raster_ms=int(raster)))

            client.log_callback = record

            def request(command):
                log.write(json.dumps({'command': command}) + '\n')
                quiet = client.log_subscribed and command.startswith(('MEMORY', 'PERF'))
                if quiet:
                    client.unsubscribe_logs(timeout=3)
                frames = client.request(command, timeout=30)
                for frame in frames:
                    if frame.kind != 'DATA' or command.startswith(('MEMORY', 'PERF')):
                        record(frame)
                if quiet:
                    client.subscribe_logs()
                return frames

            def pump(seconds):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    client.pump_logs()
                    time.sleep(.01)

            def memory():
                return [pxadb.info_properties(f.payload) for f in request('MEMORY') if f.kind == 'DATA']

            running = monitoring = recording = False
            try:
                report['hello'] = client.hello()
                assert 'heap-local' in report['hello']
                assert not any('active=1' in f.payload for f in request('PACKAGES') if f.kind == 'PKG')
                client.subscribe_logs()
                pump(.2)
                for index in range(args.repeat):
                    request('INPUT KEY HOME'); pump(.3)
                    samples.clear(); accepting = False
                    request('MEMORY START'); monitoring = True
                    before = memory()
                    request('PERF START'); recording = True
                    request(f'PACKAGE run {args.app}'); running = True
                    end = time.monotonic() + 180
                    while len(samples) < 11 and time.monotonic() < end:
                        pump(.1)
                    assert [s['index'] for s in samples] == list(range(1,12)), samples
                    raster = [f.payload for f in request('PERF STOP')]; recording = False
                    after = memory()
                    request('MEMORY STOP'); monitoring = False
                    row = dict(index=index, groups=list(samples), memory_before=before,
                               memory_after=after, raster=raster)
                    report['runs'].append(row)
                    # Preserve the expensive measurement even if debug image
                    # transfer fails; it must not be mislabeled as a full pass.
                    (args.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
                    client.unsubscribe_logs(timeout=3)
                    for retry in range(6):
                        try:
                            shot = pxadb.screenshot_capture(request('SCREENSHOT JPEG'))
                            break
                        except pxadb.PxaDbError as error:
                            if retry == 5 or not pxadb.screenshot_retryable(error):
                                raise
                            pump(.2)
                    pxadb.write_screenshot(shot, args.output / f'final-{index}.jpg')
                    row['screenshot'] = shot.metadata
                    client.subscribe_logs()
                    request(f'PACKAGE stop {args.app}'); running = False
                    pump(.3)
                    row['stopped'] = memory()
                    (args.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
                    print(json.dumps(row['groups']), flush=True)
                report['passed'] = True
            finally:
                if recording: request('PERF CLEAR')
                if monitoring: request('MEMORY STOP')
                if running: request(f'PACKAGE stop {args.app}')
                (args.output / 'report.json').write_text(json.dumps(report, indent=2)+'\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
