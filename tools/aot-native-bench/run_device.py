#!/usr/bin/env python3
"""Run paired native/AOT kernels, restoring the actual backed-up app in finally."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import time
import serial


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--chip', choices=['esp32s3', 'esp32s31'], required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--partition-table', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--boots', type=int, default=2)
    args = parser.parse_args()
    if args.boots < 1:
        parser.error('--boots must be positive')
    apps = []
    partition_bytes = args.partition_table.read_bytes()
    for at in range(0, len(partition_bytes), 32):
        magic, kind, sub, offset, size, _, _ = struct.unpack_from('<HBBII16sI', partition_bytes, at)
        if magic != 0x50aa:
            break
        if kind == 0:
            apps.append((sub, offset, size))
    if len(apps) != 1 or apps[0][0] != 0:
        raise RuntimeError('An unambiguous factory partition is required')
    _, offset, size = apps[0]
    binary = args.build_dir / 'native_aot_bench.bin'
    if args.backup.stat().st_size != size or args.backup.read_bytes()[0] != 0xe9:
        raise RuntimeError('Backup must contain the entire actual app partition')
    if not binary.is_file() or binary.stat().st_size > size or binary.read_bytes()[0] != 0xe9:
        raise RuntimeError('Invalid diagnostic app binary')
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(chip=args.chip, port=args.port, passed=False, restored=False,
                  offset=offset, partition_size=size, backup_sha256=sha(args.backup),
                  diagnostic_sha256=sha(binary), metadata=[], rows=[])

    def save():
        (args.output/'result.json').write_text(json.dumps(report, indent=2)+'\n')

    def flash(path, label):
        command = [sys.executable, '-m', 'esptool', '--chip', args.chip,
                   '--port', args.port, '--baud', '2000000', '--after', 'hard-reset',
                   'write-flash', '--flash-mode', 'keep', '--flash-size', 'keep',
                   '--flash-freq', 'keep', hex(offset), str(path)]
        with (args.output/(label+'.log')).open('wb') as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)

    try:
        save()
        for boot in range(args.boots):
            print(f'{args.chip}: flashing diagnostic boot {boot+1}', flush=True)
            flash(binary, f'flash-{boot}')
            device = serial.Serial(port=None, baudrate=115200, timeout=.2)
            device.dtr = False
            device.rts = False
            device.port = args.port
            deadline = time.monotonic()+10
            while True:
                try:
                    device.open()
                    break
                except serial.SerialException:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.2)
            count = 0
            with device, (args.output/f'serial-{boot}.log').open('w', buffering=1) as log:
                deadline = time.monotonic()+240
                while time.monotonic() < deadline:
                    line = device.readline().decode('utf-8', 'replace').strip()
                    if not line:
                        continue
                    log.write(line+'\n')
                    if line.startswith('BENCH_META '):
                        row = json.loads(line[len('BENCH_META '):])
                        if row['target'] != args.chip:
                            raise RuntimeError('Unexpected diagnostic target')
                        row['boot'] = boot
                        report['metadata'].append(row)
                        print(row, flush=True)
                    elif line.startswith('BENCH_ROW '):
                        row = json.loads(line[len('BENCH_ROW '):])
                        row['boot'] = boot
                        report['rows'].append(row)
                        count += 1
                        save()
                    elif line.startswith('BENCH_ERROR') or 'Guru Meditation' in line or 'assert failed' in line:
                        raise RuntimeError(line)
                    elif line == 'BENCH_DONE':
                        break
                else:
                    raise RuntimeError('Diagnostic timed out; inspect serial log')
            if count != 56:
                raise RuntimeError(f'Incomplete boot: {count}/56 samples')
            print(f'{args.chip}: boot {boot+1} completed, all checksums matched', flush=True)
        groups = {}
        for row in report['rows']:
            groups.setdefault(row['kernel'], []).append(row)
        summary = {}
        for kernel, rows in groups.items():
            if len(rows) != args.boots*7:
                raise RuntimeError('Incomplete sample group')
            native = statistics.median(r['native_us'] for r in rows)
            aot = statistics.median(r['aot_us'] for r in rows)
            summary[kernel] = dict(native_us=native, aot_us=aot,
                                   aot_over_native=statistics.median(
                                       r['aot_us']/r['native_us'] for r in rows),
                                   native_ns_per_iteration=statistics.median(
                                       r['native_us']*1000/r['iterations'] for r in rows),
                                   aot_ns_per_iteration=statistics.median(
                                       r['aot_us']*1000/r['iterations'] for r in rows),
                                   samples=len(rows),
                                   iterations=sorted({r['iterations'] for r in rows}))
        report['summary'] = summary
        report['passed'] = True
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        print(f'{args.chip}: restoring actual original application partition', flush=True)
        if sha(args.backup) != report['backup_sha256']:
            raise RuntimeError('Backup changed during benchmark; refusing restore')
        try:
            flash(args.backup, 'restore-original')
            report['restored'] = True
            print(f'{args.chip}: original application restored and flash hash verified', flush=True)
        finally:
            save()
    print(json.dumps(report['summary'], indent=2), flush=True)


if __name__ == '__main__':
    main()
