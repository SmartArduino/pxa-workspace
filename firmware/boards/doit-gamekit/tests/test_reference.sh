#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
python3 - "$project_dir" "${1:-product-lvgl96}" <<'PY'
import os, re, shlex, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
build = root / 'build/simulator' / sys.argv[2]
flags_path = build / 'CMakeFiles/pxsys_desktop_simulator.dir/flags.make'
if not flags_path.exists():
    raise SystemExit('Build the specified simulator profile first.')
output = root / 'build/tests/doit-gamekit'
output.mkdir(parents=True, exist_ok=True)
flags = flags_path.read_text()
arguments = []
for variable in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'):
    arguments += shlex.split(re.search('^' + variable + r' = (.*)$', flags, re.M)[1])
arguments = [value for value in arguments if value != '-DNDEBUG' and not value.startswith('-O')]
arguments += ['-O1']
sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
libraries = ['system/standard/libpxa_system_standard.a',
             'system/runtimes/native/libpxa_system_native.a',
             'system/core/libpxa_system_core.a',
             'ui/reference/libpxa_system_reference_layout.a',
             'ui/reference/lvgl/libpxa_system_reference_lvgl.a',
             'libpxa/libpxa_adapters_lvgl.a', 'libpxa/libpxa.a']
platform_libraries = shlex.split(subprocess.check_output(
    ['pkg-config', '--libs', 'sdl2', 'freetype2'], text=True))
for settings in (0, 1):
    obj = output / f'reference_features_{settings}.o'
    binary = output / f'reference_features_{settings}'
    subprocess.run(['cc', *arguments, *sanitizers, '-Werror',
                    f'-DPXSYS_REFERENCE_UI_BUILTIN_SETTINGS={settings}', '-c',
                    str(root / 'deps/pxa-system/ui/reference/tests/reference_features_test.c'),
                    '-o', str(obj)], check=True)
    subprocess.run(['cc', *sanitizers, str(obj), '-o', str(binary), '-Wl,--start-group',
                    *[str(build / 'pxa-system' / item) for item in libraries], '-Wl,--end-group',
                    str(build / 'lvgl/liblvgl.a'), *platform_libraries, '-lm', '-lz', '-llz4'], check=True)
    subprocess.run([str(binary)], env={**os.environ, 'SDL_VIDEODRIVER': 'dummy',
                                      'SDL_AUDIODRIVER': 'dummy'}, check=True)
PY
