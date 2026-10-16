#!/usr/bin/env python3
"""Compile the production presenter with performance logging enabled/disabled.

Run after building pai-touch and under .pxa/firmware-build.lock, since board
profiles share ESP-IDF managed dependencies. No device or sdkconfig mutation.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

workspace = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=workspace/'build/firmware/pai-touch')
parser.add_argument('--baseline-header', type=Path,
                    help='Optional earlier parallel_sw_rotation_flush.h for Context size comparison.')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
entries = json.loads((args.build_dir/'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/pai_touch_board_port.cc'))
arguments = shlex.split(entry['command'])
compiler_args = []
skip = False
for argument in arguments:
    if skip:
        skip = False
    elif argument in ('-o', '-MF', '-MT', '-MQ'):
        skip = True
    elif argument not in ('-c', '-MD', '-MMD', '-MP', entry['file']):
        compiler_args.append(argument)
header = workspace/'firmware/boards/pai-touch/include/parallel_sw_rotation_flush.h'
# Read dependencies before exposing private members for the size probe. This
# leaves standard library and SDK header access specifiers untouched.
includes = '\n'.join(line for line in header.read_text().splitlines()
                     if line.startswith('#include'))
cases = [('logging', header, 1), ('no-logging', header, 0)]
if args.baseline_header:
    cases.append(('baseline', args.baseline_header.resolve(), 1))
result = {}
with tempfile.TemporaryDirectory(prefix='pai-performance-') as temp:
    for name, source_header, enabled in cases:
        source = Path(temp)/(name+'.cc')
        assembly = source.with_suffix('.s')
        source.write_text(includes + '\n#define private public\n#include "' +
                          str(source_header) + '"\n#undef private\n' +
                          'extern "C" const unsigned performance_context_size = '
                          'sizeof(zuowei_pai_touch::ParallelSoftwareRotationFlush::Context);\n')
        command = [a.replace('-DCONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG=1',
                             '-DCONFIG_ZUOWEI_PAI_TOUCH_DISPLAY_PERF_LOG='+str(enabled))
                   for a in compiler_args]
        assert any(a.endswith('DISPLAY_PERF_LOG='+str(enabled)) for a in command)
        subprocess.run([*command, '-S', str(source), '-o', str(assembly)],
                       cwd=entry['directory'], check=True)
        size = re.search(r'performance_context_size:\s*\.(?:long|word|4byte)\s+(\d+)',
                         assembly.read_text())
        assert size, 'Context size not found in generated assembly'
        result[name] = int(size[1])
if args.baseline_header:
    assert result['logging'] <= result['baseline'], result
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
print('pai-touch production presenter compiled with PERF=1/0; Context bytes:', result)
