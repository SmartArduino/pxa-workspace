#!/usr/bin/env python3
"""Verify the WASI equivalent of upstream Quake II's float-constant option."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output / 'constants.c'
    source.write_text('float implicit(float x) { return x * 0.1 + 1.0; }\n'
                      'double explicit_double(double x) { return x * (double)0.1f; }\n')
    flags = ['-Xclang', '-cl-single-precision-constant']
    report = {'device_fps_measured': False, 'flags': flags, 'passed': False}
    for name, extra in [('default', []), ('upstream_precision', flags)]:
        result = subprocess.run([str(args.clang), '-O0', '-S', '-emit-llvm',
                                 *extra, str(source), '-o', '-'],
                                check=True, text=True, capture_output=True)
        (args.output / (name + '.ll')).write_text(result.stdout)
        body = re.search(r'define .*?@implicit\(.*?\n}', result.stdout, re.S).group()
        double_body = re.search(r'define .*?@explicit_double\(.*?\n}',
                                result.stdout, re.S).group()
        report[name] = {'implicit_uses_f64': 'fmuladd.f64' in body,
                        'implicit_uses_f32': 'fmuladd.f32' in body,
                        'explicit_double_preserved': 'fmul double' in double_body,
                        'diagnostics': result.stderr}
        assert report[name]['explicit_double_preserved']
        assert not result.stderr, result.stderr
    assert report['default']['implicit_uses_f64']
    assert report['upstream_precision']['implicit_uses_f32']
    assert not report['upstream_precision']['implicit_uses_f64']
    report.update(passed=True,
                  compiler_sha256=hashlib.sha256(args.clang.resolve().read_bytes()).hexdigest(),
                  compiler_version=subprocess.check_output([str(args.clang), '--version'], text=True))
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: implicit float expressions stay f32; explicit double stays f64. No FPS claim.')


if __name__ == '__main__':
    main()
