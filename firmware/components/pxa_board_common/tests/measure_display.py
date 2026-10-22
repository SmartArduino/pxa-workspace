#!/usr/bin/env python3
"""Measure completed LCD DMA frames, keeping captures outside the timed window.

Install tests/apps/display-benchmark first. Use --existing for a running game.
This measures completed transfers, not optical panel scan Hz or physical tearing.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--serial', required=True)
parser.add_argument('--board', required=True, choices=['doit-gamekit', 'doit-gamekit-1'])
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--samples', type=int, default=3)
parser.add_argument('--existing', action='store_true')
parser.add_argument('--app-id', default='pxa-display-benchmark')
args = parser.parse_args()
if args.seconds <= 0 or args.samples < 1:
    parser.error('Positive seconds and sample count required')
os.environ['PXADB_BAUD'] = '2000000'
sys.path.insert(0, str(Path(__file__).resolve().parents[4] / 'tools/pxadb'))
import pxadb

args.output.mkdir(parents=True, exist_ok=True)
result = {'scope': 'completed LCD DMA frames; not optical scan Hz', 'samples': [], 'app_id': args.app_id}
with (args.output / 'serial.log').open('w', buffering=1) as log, pxadb.PxaDbClient(args.port, 30) as client:
    client.log_callback = lambda frame: log.write(frame.payload + '\n')
    client.raw_callback = lambda data: log.write(data.decode('utf8', 'replace'))
    hello = pxadb.info_properties(client.hello())
    assert hello['serial'] == args.serial and hello['target'] == 'esp32s31', hello
    info = client.request('INFO')
    props = pxadb.info_properties(next(x.payload for x in info if x.kind == 'OK'))
    assert props['board'] == args.board, props
    result['info'] = props
    result['packages'] = [x.payload for x in client.request('PACKAGES') if x.kind == 'PKG']
    result['memory_before'] = [x.payload for x in client.request('MEMORY')]
    client.subscribe_logs()
    def pump(seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            client.pump_logs()
            time.sleep(min(0.02, max(0, end-time.monotonic())))
    try:
        if not args.existing:
            client.request('INPUT SWIPE 160 212 160 48 400 12')
            pump(0.8)
            try:
                client.request('PACKAGE stop '+args.app_id)
            except pxadb.PxaDbError as error:
                if 'package_not_running' not in str(error):
                    raise
            pump(1)
            client.request('PACKAGE run '+args.app_id)
        pump(4)
        packages = client.request('PACKAGES')
        assert any('active=1' in f.payload and (args.existing or args.app_id+'\t' in f.payload) for f in packages), packages
        for _ in range(args.samples):
            client.request('PERF START display')
            pump(args.seconds)
            frames = client.request('PERF STOP')
            values = pxadb.info_properties(next(x.payload for x in frames if x.kind == 'OK'))
            assert values['display'] == '1' and int(values['window_us']) > 0, values
            values['fps'] = int(values['completed']) * 1000000 / int(values['window_us'])
            result['samples'].append(values)
            print(json.dumps(values), flush=True)
        pxadb.write_screenshot(pxadb.screenshot_capture(client.request('SCREENSHOT JPEG', timeout=30)), args.output / 'frame.jpg')
        result['memory_after'] = [x.payload for x in client.request('MEMORY')]
    finally:
        client.request('INPUT CANCEL')
        (args.output/'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')
