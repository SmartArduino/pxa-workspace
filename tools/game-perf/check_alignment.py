#!/usr/bin/env python3
"""Check the isolated AOT alignment experiment; never changes SDK defaults."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess


def run(args):
    subprocess.run([str(a) for a in args], check=True)


def wasm_functions(data):
    def leb(at):
        value = shift = 0
        while True:
            byte = data[at]; at += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value, at
            shift += 7
    at = 8
    exports = {}
    while at < len(data):
        kind = data[at]; size, at = leb(at + 1); end = at + size
        if kind == 2:
            assert leb(at)[0] == 0, 'Probe must have no imported functions'
        if kind == 3:
            count, _ = leb(at)
        if kind == 7:
            n, cursor = leb(at)
            for _ in range(n):
                size, cursor = leb(cursor)
                name = data[cursor:cursor + size].decode(); cursor += size
                export_kind = data[cursor]; index, cursor = leb(cursor + 1)
                if export_kind == 0:
                    exports[name] = index
        at = end
    return count, exports


def codegen(path, count, exports, objdump):
    data = path.read_bytes(); at = 8; sections = {}
    while at < len(data):
        kind, size = struct.unpack_from('<II', data, at); at += 8
        sections[kind] = data[at:at + size]; at = (at + size + 3) & ~3
    assert len(sections[3]) == count * 16, 'Unexpected pinned RV32 AOT format'
    offsets = struct.unpack_from('<' + str(count) + 'I', sections[3])
    text = sections[2][4 + struct.unpack_from('<I', sections[2])[0]:]
    raw = path.with_suffix('.text.bin'); raw.write_bytes(text)
    ends = dict(zip(sorted(offsets), sorted(offsets)[1:] + [len(text)]))
    results = {}
    for name, index in exports.items():
        start = offsets[index]; end = ends[start]
        asm = subprocess.check_output([
            str(objdump), '-D', '-b', 'binary', '-m', 'riscv:rv32',
            f'--start-address={start}', f'--stop-address={end}', str(raw)], text=True)
        path.with_suffix('.' + name + '.asm').write_text(asm)
        results[name] = {'bytes': end - start, **{
            op: len(re.findall(r'\t' + op + r'\s', asm))
            for op in ('lbu', 'sb', 'lw', 'sw', 'bltu')}}
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidate_wamrc', 'baseline_wamrc', 'wasi_sdk', 'runtime_library', 'objdump', 'output'):
        parser.add_argument('--' + name.replace('_', '-'), required=True, type=Path)
    parser.add_argument('--require-loop-alignment', action='store_true',
                        help='Require proof of alignment through loop-carried pointers')
    parser.add_argument('--require-checked-word-access', action='store_true',
                        help='Require an additional word fast path for unknown alignment')
    parser.add_argument('--candidate-opt-level', choices=('0', '1', '2', '3', 's', 'z'),
                        help='Override candidate optimization; defaults remain RV32 O3 and x86 O2')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    wasm = out / 'probe.wasm'
    run([args.wasi_sdk / 'bin/clang', '-O2', '-nostdlib', '-Wl,--no-entry',
         '-Wl,--initial-memory=131072', '-Wl,--max-memory=196608',
         here / 'alignment_probe.c', '-o', wasm])
    count, exports = wasm_functions(wasm.read_bytes())
    flags = ['--target=riscv32', '--target-abi=ilp32f', '--cpu=generic-rv32',
             '--cpu-features=+m,+a,+f,+c,+zba,+zbb,+zbs', '--size-level=3']
    results = {}
    for label, compiler in (('baseline', args.baseline_wamrc), ('candidate', args.candidate_wamrc)):
        aot = out / (label + '.rv32.aot')
        level = (args.candidate_opt_level or '3') if label == 'candidate' else '3'
        run([compiler, *flags, '--opt-level=' + level, '-o', aot, wasm])
        results[label] = codegen(aot, count, exports, args.objdump)
    base, new = results['baseline'], results['candidate']
    assert new['load_aligned']['lbu'] < base['load_aligned']['lbu']
    assert new['load_any']['lbu'] >= 4
    assert new['store_aligned']['sb'] < base['store_aligned']['sb']
    assert new['store_any']['sb'] >= 4
    assert new['load_aligned']['bltu'] > 0
    if args.require_loop_alignment:
        assert new['load_stride']['lbu'] < base['load_stride']['lbu']
        assert new['store_stride']['sb'] < base['store_stride']['sb']
    if args.require_checked_word_access:
        assert new['load_any']['lw'] > base['load_any']['lw']
        assert new['store_any']['sw'] > base['store_any']['sw']
    x86 = out / 'candidate.x86.aot'
    run([args.candidate_wamrc, '--target=x86_64', '--opt-level=' + (args.candidate_opt_level or '2'),
         '--bounds-checks=1', '-o', x86, wasm])
    sdk = here.parents[1] / 'deps/pxa-system'
    runner = out / 'runner'
    run([*shlex.split(os.environ.get('CC', 'cc')), '-O2',
         '-I' + str(sdk / 'wamr/core/iwasm/include'), here / 'alignment_runner.c',
         args.runtime_library, '-lm', '-ldl', '-pthread', '-o', runner])
    run([runner, x86])
    results.update(passed=True, device_fps_measured=False, flags=flags,
                   candidate_opt_level=args.candidate_opt_level,
                   require_loop_alignment=args.require_loop_alignment,
                   require_checked_word_access=args.require_checked_word_access,
                   candidate_compiler_sha256=hashlib.sha256(args.candidate_wamrc.read_bytes()).hexdigest(),
                   baseline_compiler_sha256=hashlib.sha256(args.baseline_wamrc.read_bytes()).hexdigest(),
                   wasm_sha256=hashlib.sha256(wasm.read_bytes()).hexdigest())
    (out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print('PASS: aligned RV32 access shortened; unaligned access and bounds remain checked. No device FPS claim.')


if __name__ == '__main__':
    main()
