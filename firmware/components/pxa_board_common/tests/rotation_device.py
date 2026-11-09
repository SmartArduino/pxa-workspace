#!/usr/bin/env python3
"""Exercise an installed SDK orientation example using real PXADB input.

Only stops/starts the explicitly selected fixture. Run on an authorized test
device; screenshots and logs require an ignored local output directory.
"""
import sys, os, json, time, argparse, hashlib
from pathlib import Path
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/pxadb'))
import pxadb
from ui_query import UiQueryError, decode_snapshot, tap_target
p = argparse.ArgumentParser()
p.add_argument('--port', required=True)
p.add_argument('--board', required=True)
p.add_argument('--width', type=int, required=True)
p.add_argument('--height', type=int, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--elf', type=Path)
p.add_argument('--identity', default='pxa-cpp-orientation')
p.add_argument('--unlock-reference-ui', action='store_true',
               help='Wake and swipe up on the reference lock screen before testing')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
os.environ['PXADB_BAUD'] = '2000000'
identity = a.identity
report = {'board': a.board, 'angles': []}
with (out / 'console.log').open('w', buffering=1) as raw, pxadb.open_client(a.port, 12) as c:
    try:
        c.raw_callback = lambda data: raw.write(str(data) + '\n')
        c.log_callback = lambda f: raw.write(f.kind + ' ' + f.payload + '\n')
        c.subscribe_logs()
        info = c.request('INFO')[-1].payload
        (out / 'info.txt').write_text(info + '\n')
        assert 'pxa=ready' in info and 'board=' + a.board in info
        if a.elf:
            assert pxadb.info_properties(info)['elf_sha256'] == hashlib.sha256(a.elf.read_bytes()).hexdigest()
        try:
            c.request('PACKAGE stop ' + identity)
        except pxadb.PxaDbRejectedError as e:
            if str(e) != 'package_not_running':
                raise
        time.sleep(0.5)
        if a.unlock_reference_ui:
            c.request('INPUT CANCEL')
            c.request('INPUT KEY HOME')
            c.request('INPUT SYNC')
            time.sleep(0.4)
            c.request(f'INPUT SWIPE {a.width // 2} {a.height * 7 // 8} '
                      f'{a.width // 2} {a.height // 5} 300 16')
            c.request('INPUT SYNC')
            time.sleep(0.3)
        c.request('PACKAGE run ' + identity)

        def tree():
            for attempt in range(3):
                try:
                    return decode_snapshot(c.request('UI INSPECT'))
                except ValueError:
                    if attempt == 2:
                        raise
                    time.sleep(0.2)

        def wait(test):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    value = tree()
                    if test(value):
                        return value
                except (pxadb.PxaDbError, ValueError):
                    pass
                time.sleep(0.1)
            (out / 'failed-ui.json').write_text(json.dumps(tree(), indent=2))
            print([(f.kind, f.payload) for f in c.request('PACKAGES')], flush=True)
            raise AssertionError('UI did not reach expected state')

        def tap(text, pointer=False):
            for attempt in range(5):
                current = tree()
                try:
                    target = tap_target(current, {'text': text})
                    break
                except UiQueryError:
                    if pointer or attempt == 4:
                        raise
                    wanted = next(n for n in current['nodes'] if n.get('text') == text)
                    scroll = next(n['clip'] for n in current['nodes'] if n['type'] == 'scroll' and n['visible'])
                    x = scroll['x'] + scroll['width'] // 2
                    top = scroll['y'] + scroll['height'] // 4
                    bottom = scroll['y'] + scroll['height'] * 3 // 4
                    start, end = (top, bottom) if wanted['bounds']['y'] < scroll['y'] else (bottom, top)
                    c.request(f'INPUT SWIPE {x} {start} {x} {end} 200 12')
                    c.request('INPUT SYNC')
                    time.sleep(0.3)
            if pointer:
                c.request(f"INPUT POINTER DOWN {target['x']} {target['y']} 0")
                time.sleep(0.06)
                reply = c.request(f"INPUT POINTER UP {target['x']} {target['y']} 0")
            else:
                reply = c.request(f"INPUT TAP {target['x']} {target['y']}")
            c.request('INPUT SYNC')
            assert not any(('status=rejected' in f.payload for f in reply))

        def capture():
            for attempt in range(3):
                try:
                    return pxadb.screenshot_capture(c.request('SCREENSHOT JPEG AFTER_PRESENT', timeout=30))
                except pxadb.PxaDbError as e:
                    if not pxadb.screenshot_retryable(e) or attempt == 2:
                        raise
                    time.sleep(0.4)

        def expect(rotation, name):
            width, height = (a.height, a.width) if rotation & 1 else (a.width, a.height)
            value = wait(lambda t: t['viewport'] == {'width': width, 'height': height} and any((f'/ {rotation * 90} deg /' in (n.get('text') or '') for n in t['nodes'])))
            assert not any(('Display metrics error' in (n.get('text') or '') or 'Display metrics unavailable' in (n.get('text') or '') for n in value['nodes']))
            (out / (name + '-ui.json')).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
            shot = capture()
            assert int(shot.metadata['width']) == width and int(shot.metadata['height']) == height, shot.metadata
            pxadb.write_screenshot(shot, out / (name + '.png'), False)
            report['angles'].append({'name': name, 'rotation': rotation, 'size': [width, height]})
            print(a.board, name, width, height, flush=True)
        expect(0, 'native')
        for angle in (90, 180, 270, 0):
            tap(str(angle))
            expect(angle // 90, str(angle))
        tap('Landscape')
        expect(0 if a.width >= a.height else 1, 'landscape')
        tap('Portrait')
        expect(0 if a.width <= a.height else 1, 'portrait')
        tap('0')
        expect(0, 'before-held-pointer')
        c.request('INPUT POINTER DOWN 1 1 1')
        tap('90', pointer=True)
        expect(1, 'before-lock')
        time.sleep(0.15)
        c.request('INPUT POINTER DOWN 1 1 1')
        c.request('INPUT POINTER UP 1 1 1')
        c.request('INPUT SYNC')
        if a.width == a.height:
            tap('Landscape')
            expect(1, 'square-landscape-retains-angle')
            tap('Portrait')
            expect(1, 'square-portrait-retains-angle')
        tap('Lock')
        expect(1, 'locked')
        c.request('INPUT KEY HOME')
        c.request('INPUT SYNC')
        time.sleep(0.5)
        shot = capture()
        assert (int(shot.metadata['width']), int(shot.metadata['height'])) == (a.width, a.height)
        pxadb.write_screenshot(shot, out / 'home.png', False)
        c.request('PACKAGE run ' + identity)
        expect(1, 'resumed-lock')
        try:
            c.request('PACKAGE stop ' + identity)
        except pxadb.PxaDbRejectedError as e:
            if str(e) != 'package_not_running':
                raise
        time.sleep(0.5)
        shot = capture()
        assert (int(shot.metadata['width']), int(shot.metadata['height'])) == (a.width, a.height)
        pxadb.write_screenshot(shot, out / 'stopped.png', False)
        report.update(passed=True, held_pointer_cancelled=True, home_restores_system=True, resume_restores_lock=True, stop_restores_system=True, elf_verified=bool(a.elf))
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
    finally:
        for command in ('INPUT CANCEL', 'PACKAGE stop ' + identity):
            try:
                c.request(command, timeout=5)
            except (pxadb.PxaDbError, OSError):
                pass
