#!/usr/bin/env python3
"""Measure the installed camera on PAI; never capture photos or clear app data.

Run from the workspace root. Requires an idle device and an already-authorized
camera app. Waking, unlocking, starting and stopping that app are test actions.
Screenshots and timing logs are local evidence, not Store listing assets.
"""
import argparse
import contextlib
import json
from pathlib import Path
import re
import secrets
import sys
import time

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT/'tools/pxadb'))
import pxadb


def current_launch_samples(text, identity):
    marker = 'Queueing launch request: '+identity
    launch = text.rfind(marker)
    if launch < 0:
        raise ValueError('Missing current camera launch marker')
    window = text[launch:]
    ready = window.find('GC2145 ready:')
    if ready < 0:
        raise ValueError('Current launch did not initialize GC2145')
    window = window[ready:]
    samples = []
    for frames, elapsed in re.findall(r'Camera preview: frames=(\d+) elapsed_ms=(\d+)', window):
        frames, elapsed = int(frames), int(elapsed)
        if elapsed <= 0:
            raise ValueError('Invalid camera sample duration')
        samples.append(dict(frames=frames, elapsed_ms=elapsed, fps=frames*1000/elapsed))
    fields = ['frame', 'acquire_us', 'convert_us', 'alloc_us', 'pixels_us', 'return_us']
    stages = [dict(zip(fields, map(int, values))) for values in re.findall(
        r'RGB preview frame=(\d+) acquire_us=(\d+) convert_us=(\d+) '
        r'alloc_us=(\d+) pixels_us=(\d+) return_us=(\d+)', window)]
    return samples, stages


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--expected-serial', required=True)
    p.add_argument('--expected-elf', required=True)
    p.add_argument('--identity', required=True, help='exact publisher:app identity of the installed camera')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--seconds', type=float, default=22)
    p.add_argument('--screenshot', action='store_true')
    p.add_argument('--probe-controls', action='store_true', help='also measure once-per-second PING round trips')
    a = p.parse_args()
    if not 22 <= a.seconds <= 30:
        p.error('Use 22..30 seconds to collect at least three 5-second windows')
    if not re.fullmatch(r'[0-9a-f]{64}:pxa-camera', a.identity):
        p.error('Use the exact installed publisher:pxa-camera identity')
    out = a.output.resolve()
    if '/local/artifacts/' not in str(out) or (out/'result.json').exists():
        p.error('Use a new output directory under local/artifacts/')
    out.mkdir(parents=True, exist_ok=True)
    result = dict(passed=False, identity=a.identity, seconds=a.seconds,
                  metric='App completed preview frames / actual elapsed_ms; not sensor maximum FPS')
    logs = []
    with (out/'serial.jsonl').open('w', buffering=1) as log:
        def record(kind, payload):
            logs.append(payload)
            log.write(json.dumps(dict(at=time.monotonic(), kind=kind, payload=payload))+'\n')
        try:
            with pxadb.PxaDbClient(a.port, 12, baudrate=2000000) as c:
                c.sequence = secrets.randbits(30)+1
                c.raw_callback = lambda b: record('RAW', b.decode(errors='replace'))
                c.log_callback = lambda f: record(f.kind, f.payload)
                c.hello()
                info = pxadb.info_properties(c.request('INFO')[-1].payload)
                if (info.get('serial') != a.expected_serial or info.get('board') != 'pai-touch' or
                        info.get('elf_sha256') != a.expected_elf):
                    raise RuntimeError('Wrong device or firmware; no test input sent')
                result['info'] = info
                packages = [f.payload for f in c.request('PACKAGES') if f.kind == 'PKG']
                if any('active=1' in row for row in packages):
                    raise RuntimeError('Device is running an app; close it before this test')
                if not any(row.startswith(a.identity+'\t') and 'installed=1' in row and
                           'enabled=1' in row for row in packages):
                    raise RuntimeError('Camera is not installed and enabled')
                started = False
                try:
                    c.subscribe_logs()
                    width, height = int(info['width']), int(info['height'])
                    # A display-off PAI consumes the first gesture only to wake.
                    for _ in range(2):
                        c.request(f'INPUT SWIPE {width//4} {height*85//100} {width//4} {height//6} 550 25')
                        c.request('INPUT SYNC')
                    started = True
                    c.request('PACKAGE run '+a.identity)
                    deadline = time.monotonic()+a.seconds
                    next_probe = time.monotonic()+1
                    result['control_round_trip_us'] = []
                    while time.monotonic() < deadline:
                        c.pump_logs()
                        if a.probe_controls and time.monotonic() >= next_probe:
                            before = time.monotonic()
                            c.request('PING', timeout=2)
                            result['control_round_trip_us'].append((time.monotonic()-before)*1e6)
                            next_probe = time.monotonic()+1
                        time.sleep(.03)
                    c.unsubscribe_logs()
                    if a.screenshot:
                        capture = pxadb.screenshot_capture(c.request('SCREENSHOT JPEG AFTER_PRESENT', timeout=30))
                        pxadb.write_screenshot(capture, out/'preview.jpg')
                finally:
                    with contextlib.suppress(Exception):
                        c.unsubscribe_logs()
                    if started:
                        try:
                            c.request('PACKAGE stop '+a.identity)
                            result['camera_stopped'] = True
                        finally:
                            with contextlib.suppress(Exception):
                                c.request('INPUT KEY HOME')
                result['samples'], result['stages'] = current_launch_samples('\n'.join(logs), a.identity)
                if len(result['samples']) < 3:
                    raise RuntimeError('Fewer than three real preview windows; check lock screen/permission state')
                result['fps_mean'] = sum(s['fps'] for s in result['samples'])/len(result['samples'])
                result['passed'] = bool(result.get('camera_stopped'))
        except Exception as error:
            result['error'] = str(error)
        finally:
            (out/'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
