#!/usr/bin/env python3
"""Run an already built diagnostic app and always restore production app firmware.

Only offset 0x10000 is written. Do not pass a bootloader, partition table or
merged image as --restore-binary. Use the device's matching production app.
Run with the ESP-IDF Python environment (esptool and pyserial).
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import serial


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--build-dir', type=Path, required=True)
    p.add_argument('--restore-binary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
    benchmark = a.build_dir / 'pxa_jpeg_benchmark.bin'
    for binary in (benchmark, a.restore_binary):
        assert binary.is_file() and binary.read_bytes()[0] == 0xe9, f'invalid ESP app: {binary}'
    backup = a.output / 'restore-app.bin'
    if a.restore_binary.resolve() != backup.resolve(): shutil.copy2(a.restore_binary, backup)

    def flash(binary, name):
        command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', a.port,
                   '--baud', '1500000', '--before', 'default-reset', '--after', 'hard-reset',
                   'write-flash', '--flash-mode', 'dio', '--flash-size', '16MB',
                   '--flash-freq', '80m', '0x10000', str(binary)]
        with (a.output / (name + '.log')).open('wb') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    rows, blit = [], []
    try:
        flash(benchmark, 'flash-benchmark')
        with serial.Serial(a.port, 115200, timeout=.2) as device, (a.output / 'serial.log').open('w') as log:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                line = device.readline().decode(errors='replace').strip()
                if not line: continue
                log.write(line + '\n'); log.flush()
                if line.startswith('JPEG_BENCH '): rows.append(json.loads(line[11:]))
                if line.startswith('BLIT_BENCH '): blit.append(json.loads(line[11:]))
                if line == 'JPEG_BENCH_DONE': break
            else: raise RuntimeError('benchmark timed out; see serial.log')
        assert len(rows) == 21, rows
        assert all(row['leak_bytes'] == 0 for row in rows), rows
        (a.output / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
        if blit:
            assert len(blit) == 12, blit
            (a.output / 'blit-results.json').write_text(json.dumps(blit, indent=2) + '\n')
        for row in rows: print(json.dumps(row), flush=True)
    finally:
        flash(backup, 'restore-production')
        print('Production app firmware restored; app/data partitions preserved.', flush=True)


if __name__ == '__main__': main()
