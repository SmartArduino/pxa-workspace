#!/usr/bin/env python3
"""Measure the installed render-kernel-probe; never infer game FPS from it."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path[:0] = [str(Path(__file__).resolve().parents[1]),
                str(Path(__file__).resolve().parents[1] / 'pxadb')]
import pxadb
from usb_keep_awake import usb_parent
from run_main_loop_probe import require_idle_packages, log_barrier


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', required=True)
    ap.add_argument('--serial', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    if subprocess.run(['fuser', str(Path(a.port).resolve())], capture_output=True).returncode != 1:
        raise RuntimeError('Serial port is already open')
    parent = usb_parent(a.port)
    node = f"/dev/bus/usb/{int((parent/'busnum').read_text()):03}/{int((parent/'devnum').read_text()):03}"
    os.environ.setdefault('PXADB_BAUD', '2000000')
    app = 'pxa-render-kernel-probe'
    report = {'qualified': False, 'rows': []}
    owned = False
    records = []
    with open(node, 'rb', buffering=0), (a.output/'serial.jsonl').open('w', buffering=1) as log, pxadb.PxaDbClient(a.port, 10) as client:
        def record(kind, payload):
            row = {'time': time.monotonic(), 'kind': kind, 'payload': payload}
            records.append(row); log.write(json.dumps(row)+'\n')
        client.log_callback = lambda f: record(f.kind, f.payload)
        client.raw_callback = lambda raw: record('RAW', raw.decode('utf8', 'replace'))
        def request(command):
            record('COMMAND', command)
            frames = client.request(command, timeout=30)
            for f in frames: record(f.kind, f.payload)
            return frames
        def pump(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                client.pump_logs(); time.sleep(.01)
        def active(frames):
            return any(f.kind == 'PKG' and f':{app}\t' in f.payload and 'active=1' in f.payload for f in frames)
        try:
            report['hello'] = client.hello()
            if pxadb.info_properties(report['hello'])['serial'] != a.serial:
                raise RuntimeError('Wrong MCU')
            report['info'] = pxadb.info_properties(request('INFO')[-1].payload)
            if report['info']['elf_sha256'] != a.elf:
                raise RuntimeError('Wrong firmware')
            require_idle_packages(request('PACKAGES'))
            log_barrier(request('INPUT SYNC'))
            report['memory_before'] = [f.payload for f in request('MEMORY')]
            owned = True
            client.subscribe_logs()
            request('INPUT KEY HOME'); pump(.2)
            request('INPUT POINTER DOWN 160 185 0'); pump(.1)
            for y in [163, 139, 115, 91, 67, 53]:
                request(f'INPUT POINTER MOVE 160 {y} 0'); pump(.1)
            request('INPUT POINTER UP 160 53 0'); pump(.2)
            barrier = log_barrier(request('INPUT SYNC'))
            report['log_barrier'] = barrier
            request(f'PACKAGE run {app}')
            until = time.monotonic()+240
            rows = {}
            while time.monotonic() < until:
                pump(.2)
                for row in records:
                    if row['kind'] != 'LOG': continue
                    m = re.match(r'^(\d+)\t', row['payload'])
                    if not m or not 0 < ((int(m[1])-barrier)&0xffffffff) < 0x80000000: continue
                    if f'[PXA app={app} ' not in row['payload']: continue
                    if 'RENDERBENCH round=' not in row['payload']: continue
                    fields = dict(re.findall(r'(\w+)=([0-9a-f]+)', row['payload'].split('RENDERBENCH ', 1)[1]))
                    key = (int(fields['round']), int(fields['scene']))
                    fields = {k: int(v, 16 if k == 'hash' else 10) for k, v in fields.items()}
                    if key in rows and rows[key] != fields: raise RuntimeError('Conflicting benchmark records')
                    rows[key] = fields
                if len(rows) == 24: break
            report['rows'] = [rows[k] for k in sorted(rows)]
            if set(rows) != {(r,s) for r in range(3) for s in range(8)}:
                raise RuntimeError(f'Incomplete benchmark: {len(rows)}/24')
            for row in report['rows']:
                pixels = 720 if row['scene']%4 == 3 else 61440
                if row['batches'] != 100 or row['rejected'] or row['pixels'] != 100*pixels:
                    raise RuntimeError('Rejected/incomplete execution')
            for scene in range(8):
                if len({rows[r,scene]['hash'] for r in range(3)}) != 1:
                    raise RuntimeError('Output changed between repeats')
            report['memory_running'] = [f.payload for f in request('MEMORY')]
            report['qualified'] = True
        except BaseException as e:
            report['error'] = repr(e)
            raise
        finally:
            try:
                if owned:
                    stopped = False
                    request('INPUT CANCEL')
                    for attempt in range(20):
                        if not active(request('PACKAGES')): stopped = True; break
                        try: request(f'PACKAGE stop {app}')
                        except pxadb.PxaDbError as e:
                            report.setdefault('stop_retries', []).append(str(e))
                        pump(.25)
                    report['stopped'] = stopped
                    if not stopped: report['qualified'] = False
                    else:
                        request('INPUT KEY HOME')
                        report['memory_after'] = [f.payload for f in request('MEMORY')]
            except Exception as e:
                report['cleanup_error'] = repr(e)
                report['qualified'] = False
            (a.output/'measurement.json').write_text(json.dumps(report, indent=2)+'\n')
            print(json.dumps(report, indent=2))

    return 0 if report['qualified'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
