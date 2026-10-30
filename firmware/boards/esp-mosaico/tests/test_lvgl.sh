#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir=${PXA_TEST_ARTIFACT_DIR:-"$project_dir/local/artifacts/mosaico-lvgl/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$build_dir"
if [[ "${1:-}" == --gestures ]]; then
  gesture_build="$build_dir/gesture-build"
  cmake -S "$tests_dir/lvgl_host" -B "$gesture_build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$gesture_build" --target mosaico_test mosaico_direct_refresh_test --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$gesture_build/mosaico_test"
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$gesture_build/mosaico_direct_refresh_test"
  exit
fi
sim_build="${1:-$project_dir/build/simulator/esp-mosaico}"
python3 - "$tests_dir" "$sim_build" "$build_dir" <<'PY'
import os, re, shlex, subprocess, sys
from pathlib import Path
tests, simulator, build = map(Path, sys.argv[1:])
flags = (simulator/'lvgl/CMakeFiles/lvgl.dir/flags.make').read_text()
arguments = []
for name in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
    arguments += shlex.split(re.search('^'+name+r' = (.*)$', flags, re.M)[1])
arguments = [a for a in arguments if a != '-DNDEBUG']
libraries = shlex.split(subprocess.check_output(
    ['pkg-config', '--libs', 'sdl2', 'freetype2', 'libpng'], text=True))
for source in ('lvgl_partial_test.cc', 'lvgl_direct_refresh_test.cc'):
    binary=build/Path(source).stem
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *arguments,
        '-Wall', '-Wextra', '-Werror', '-I'+str(tests/'../include'),
        str(tests/source), str(simulator/'lvgl/liblvgl.a'),
        *libraries, '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)],
        env={**os.environ, 'SDL_VIDEODRIVER':'dummy', 'SDL_AUDIODRIVER':'dummy'}, check=True)
PY
