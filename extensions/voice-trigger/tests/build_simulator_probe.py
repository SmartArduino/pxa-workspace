#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build an optional extension fixture using a configured product simulator."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--artifacts', type=Path, required=True)
args = parser.parse_args()
build, output = args.build.resolve(), args.artifacts.resolve()
output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[1]
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if Path(e['file']).name == 'product_runner.c'
             and 'pxsys_product_simulator.dir' in e['command'])
objects = []
for source in (root / 'tests/simulator_probe.c', root / 'src/voice_trigger.c'):
    command = shlex.split(entry['command'])
    obj = output / (source.stem + '.o')
    command[command.index('-o') + 1] = str(obj)
    command[command.index(entry['file'])] = str(source)
    command.extend(['-I' + str(root / 'include'), '-DPXA_PRODUCT_RUNNER="' + entry['file'] + '"'])
    subprocess.run(command, cwd=entry['directory'], check=True)
    objects.append(str(obj))
link = shlex.split((build / 'CMakeFiles/pxsys_product_simulator.dir/link.txt').read_text())
index = next(i for i, value in enumerate(link) if value.endswith('/product_runner.c.o'))
link[index:index + 1] = objects
link[link.index('-o') + 1] = str(output / 'voice-trigger-simulator')
subprocess.run(link, cwd=build, check=True)
print(output / 'voice-trigger-simulator')
