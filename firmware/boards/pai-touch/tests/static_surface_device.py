#!/usr/bin/env python3
"""Check one-shot direct Surface presentation after a 180-degree rotation.

Install tests/apps/static-surface first. Uses PXADB and Pillow; output belongs
in an ignored local artifact directory. Does not uninstall or erase any apps.
"""
import argparse
import io
import json
import os
from pathlib import Path
import sys
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools/pxadb'))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault('PXADB_BAUD', '2000000')
    identity = 'pxa-rot-surface'
    report = {'frames': []}
    with pxadb.open_client(args.port, 15) as client:
        try:
            try:
                client.request('PACKAGE stop ' + identity)
            except pxadb.PxaDbRejectedError as error:
                if str(error) != 'package_not_running':
                    raise
            client.request('PACKAGE run ' + identity)
            deadline = time.monotonic() + 30
            last = None
            while time.monotonic() < deadline:
                try:
                    shot = pxadb.screenshot_capture(client.request(
                        'SCREENSHOT JPEG', timeout=30))
                    last = shot.metadata
                    if last.get('source') == 'surface' and last.get('frame_id') == '2':
                        break
                except pxadb.PxaDbError as error:
                    if (not pxadb.screenshot_retryable(error) and
                            str(error) not in ('screenshot_failed', 'screenshot_rate_limited')):
                        raise
                time.sleep(0.2)
            else:
                raise AssertionError(f'Static rotated frame was not presented: {last}')
            expected = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
            for index in range(3):
                if index:
                    time.sleep(0.5)
                    shot = pxadb.screenshot_capture(client.request('SCREENSHOT JPEG'))
                assert shot.metadata['source'] == 'surface', shot.metadata
                assert shot.metadata['frame_id'] == '2', shot.metadata
                image = Image.open(io.BytesIO(shot.data)).convert('RGB')
                assert image.size == (296, 240), image.size
                samples = [image.getpixel((image.width * x // 4, image.height * y // 4))
                           for y in (1, 3) for x in (1, 3)]
                for actual, wanted in zip(samples, expected):
                    assert max(abs(a - b) for a, b in zip(actual, wanted)) < 25, samples
                pxadb.write_screenshot(shot, args.output / f'frame-{index}.png', False)
                report['frames'].append({'metadata': shot.metadata, 'quadrants': samples})
            report.update(passed=True, rotation=2, one_shot_frame=2,
                          same_surface_retained=True)
        finally:
            client.request('PACKAGE stop ' + identity)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
