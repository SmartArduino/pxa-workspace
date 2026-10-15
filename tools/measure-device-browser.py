#!/usr/bin/env python3
"""Measure Canvas browser panel completions against tests/fixture.py on pai-touch.

Requires PERF START display firmware and a browser package built with
PXA_BROWSER_START_URL=http://<fixture-host>:31878/. Optional PXA_BROWSER_PROFILE
adds aggregate Guest encode/layout/submit timings. Screenshots and heap walks
are outside the timed window. This is completed LCD transfer rate, not optical
refresh or network-independent page-load throughput. No settings are changed.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent / 'pxadb'))
import pxadb


def distribution(samples):
    if not samples:
        return {'count': 0}
    ordered = sorted(samples)
    return dict(count=len(samples), mean_us=statistics.mean(samples),
                median_us=statistics.median(samples),
                p95_us=ordered[math.ceil(len(samples)*.95)-1], max_us=ordered[-1])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--app', default='pxa-browser')
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--swipes', type=int, choices=[1,2], default=1,
                   help='one avoids including a deliberate between-gesture pause in frame intervals')
    p.add_argument('--scenes', nargs='+', choices=['idle', 'scroll', 'edge', 'loading'],
                   default=['idle', 'scroll', 'edge', 'loading'])
    a = p.parse_args()
    if a.repeat < 1:
        p.error('repeat must be positive')
    a.output.mkdir(parents=True, exist_ok=True)
    report = dict(package_sha256=hashlib.sha256(a.package.read_bytes()).hexdigest(),
                  geometry='pai-touch 296x240, density=1, font=15dp',
                  scroll=f'{a.swipes} device-timed (150,170)->(150,80) swipes, 1600ms/96 steps each',
                  scope='all completed LCD transfers, including system UI; not optical refresh',
                  runs=[], passed=False)
    records = []
    with pxadb.PxaDbClient(a.port, 30) as c, (a.output/'serial.jsonl').open('w', buffering=1) as log:
        epoch = time.monotonic()
        def record(kind, value):
            entry = dict(seconds=time.monotonic()-epoch, kind=kind, payload=value)
            log.write(json.dumps(entry, ensure_ascii=False)+'\n')
            records.append(entry)
        c.log_callback = lambda f: record(f.kind, f.payload)
        c.raw_callback = lambda b: record('RAW', b.decode(errors='replace'))
        def cmd(text):
            record('COMMAND', text)
            frames = c.request(text, timeout=30)
            for f in frames:
                if not text.startswith('SCREENSHOT') or f.kind != 'DATA':
                    record(f.kind, f.payload)
            return frames
        def props(text):
            result = {}
            for f in cmd(text):
                if f.kind in ('DATA','OK') and '=' in f.payload:
                    result.update(pxadb.info_properties(f.payload))
            return result
        def memory(text='MEMORY'):
            result = {}
            for f in cmd(text):
                if f.kind == 'DATA':
                    v = pxadb.info_properties(f.payload)
                    scope = v.pop('scope')
                    name = v.pop('name', '')
                    result[scope+(':'+name if name else '')] = {k:int(x) for k,x in v.items()}
            return result
        def pump(seconds):
            end = time.monotonic()+seconds
            while time.monotonic() < end:
                c.pump_logs()
                time.sleep(.005)
        def wait_done(mark, timeout=12):
            end = time.monotonic()+timeout
            while not any('BROWSER DONE status=200' in r['payload'] for r in records[mark:]):
                assert time.monotonic() < end, 'page did not finish; see serial.jsonl'
                pump(.05)
            pump(.4)
        def tap(x,y):
            cmd(f'INPUT TOUCH DOWN {x} {y} 0');pump(.12)
            cmd(f'INPUT TOUCH UP {x} {y} 0');pump(.15)
        def screenshot(name):
            for attempt in range(3):
                try:
                    shot = pxadb.screenshot_capture(cmd('SCREENSHOT'))
                    break
                except pxadb.PxaDbError as error:
                    if attempt == 2 or not pxadb.screenshot_retryable(error):
                        raise
                    pump(.25)
            assert [int(shot.metadata[k]) for k in ('width','height')] == [296,240]
            pxadb.write_screenshot(shot, a.output/(name+'.png'))
            return shot.metadata
        def save():
            (a.output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
        report['hello'] = c.hello();c.subscribe_logs()
        try:
            for repeat in range(a.repeat):
                for scene in a.scenes:
                    for f in cmd('PACKAGES'):
                        if f.kind == 'PKG' and 'active=1' in f.payload:
                            assert a.app in f.payload, 'another application is active'
                            # Stop is queued; the Host allows 1200ms for
                            # graceful shutdown. Keep teardown outside startup
                            # heap windows instead of treating the ACK as exit.
                            cmd('PACKAGE stop '+a.app);pump(1.5)
                    cmd('MEMORY START');launch_before=memory()
                    mark=len(records);cmd('PACKAGE run '+a.app);wait_done(mark)
                    if scene != 'loading':
                        mark=len(records);tap(50,165);wait_done(mark)
                        assert any('BROWSER NAV ' in r['payload'] and '/large' in r['payload']
                                   for r in records[mark:]), 'expected fixture /large; check font=15dp'
                    startup=memory('MEMORY STOP')
                    name=f'{repeat}-{scene}'
                    screenshot(name+'-before');pump(.4)
                    cmd('MEMORY START');before=memory()
                    mark=len(records);cmd('PERF START display')
                    if scene in ('scroll','edge'):
                        for _ in range(a.swipes):
                            points='150 170 150 80' if scene=='scroll' else '150 80 150 170'
                            cmd('INPUT SWIPE '+points+' 1600 96')
                            pump(.1)
                    elif scene == 'loading':
                        tap(120,143);pump(5.1)
                    else:
                        pump(3.4)
                    perf={k:int(v) for k,v in props('PERF STOP').items()}
                    after=memory('MEMORY STOP')
                    assert perf['display']==1 and perf['window_us']>0, perf
                    assert not any(perf[k] for k in ('present_overflow','raster_overflow','surface_changed')),perf
                    intervals=[]
                    for offset in range(0,perf['present'],16):
                        v=props(f'PERF READ present {offset}')
                        samples=[int(x) for x in v['values'].split(',') if x]
                        assert int(v['offset'])==offset and len(samples)==int(v['count'])
                        intervals.extend(samples)
                    assert len(intervals)==perf['present']
                    assert perf['completed']==(len(intervals)+1 if intervals else perf['completed'])
                    row=dict(repeat=repeat,scene=scene,launch_before=launch_before,startup=startup,
                             before=before,memory=after,perf=perf,present_us=intervals,
                             interval=distribution(intervals),
                             window_fps=perf['completed']*1e6/perf['window_us'],
                             active_fps=len(intervals)*1e6/sum(intervals) if intervals else None,
                             logs=[r for r in records[mark:] if r['kind'] in ('LOG','RAW')])
                    cmd('PERF CLEAR');row['post_probe']=memory()
                    assert any(f.kind=='PKG' and a.app in f.payload and 'active=1' in f.payload
                               for f in cmd('PACKAGES')), 'browser left the foreground during measurement'
                    screenshot(name+'-after')
                    report['runs'].append(row);save()
                    print(json.dumps({k:row[k] for k in ('repeat','scene','window_fps','active_fps','interval')}),flush=True)
                    mark=len(records);cmd('PACKAGE stop '+a.app);pump(1.5)
                    row['exit_logs']=[r for r in records[mark:] if r['kind'] in ('LOG','RAW')]
                    save()
            errors=[r for r in records if r['kind'] in ('LOG','RAW') and any(s in r['payload'] for s in
                    ('BROWSER DRAW ERROR','BROWSER ERROR','Guru Meditation','assert failed','Timed out waiting for LCD'))]
            assert not errors, errors
            report['passed']=True
        finally:
            for command in ('INPUT CANCEL','PERF CLEAR','MEMORY STOP'):
                try:cmd(command)
                except pxadb.PxaDbError:pass
            save()
    print('browser device benchmark PASS',flush=True)


if __name__=='__main__':
    main()
