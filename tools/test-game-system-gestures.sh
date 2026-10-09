#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sim_build="$root/build/simulator/${1:-esp-mosaico}"
[[ -f "$sim_build/CMakeFiles/pxsys_desktop_simulator.dir/link.txt" ]] || {
  echo 'Build the simulator first.' >&2; exit 2;
}
test_dir="$(mktemp -d /tmp/pxa-game-gestures.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
cmake -S "$root/firmware/managed_components/lvgl__lvgl" -B "$root/build/tests/lvgl-align64" \
  -DCMAKE_BUILD_TYPE=Release \
  -DLV_BUILD_CONF_PATH="$root/firmware/components/pxa_integration/tests/lv_conf_overlay.h" \
  -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
  -DCONFIG_LV_USE_THORVG=OFF -DCONFIG_LV_USE_SDL=ON \
  -DCONFIG_LV_USE_FREETYPE=ON -DLV_BUILD_INSTALL=OFF >/dev/null
cmake --build "$root/build/tests/lvgl-align64" --target lvgl -j 6 >/dev/null
python3 - "$root" "$sim_build" "$test_dir" <<'PY'
import os, re, shlex, subprocess, sys
from pathlib import Path
root, build, temporary = map(Path, sys.argv[1:])
flags = (build/'CMakeFiles/pxsys_desktop_simulator.dir/flags.make').read_text()
arguments = []
for variable in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS'):
    arguments += shlex.split(re.search('^'+variable+r' = (.*)$', flags, re.M)[1])
arguments = [arg for arg in arguments if arg != '-DNDEBUG']
sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if os.environ.get('PXA_TEST_SANITIZERS') == '1' else []
arguments += sanitizers
subprocess.run(['cc', *arguments, '-c',
    str(root/'firmware/components/pxa_integration/tests/application_gesture_test.c'),
    '-o', str(temporary/'test.o')], check=True)
link = shlex.split((build/'CMakeFiles/pxsys_desktop_simulator.dir/link.txt').read_text())
link = [arg for arg in link if not arg.startswith('-Wl,--dependency-file=')]
link = [str(temporary/'test.o') if arg == 'CMakeFiles/pxsys_desktop_simulator.dir/main.c.o'
        else arg for arg in link]
link[link.index('-o')+1] = str(temporary/'test')
link += sanitizers
subprocess.run(link, cwd=build, check=True)
subprocess.run([str(temporary/'test')],
    env={**os.environ, 'SDL_VIDEODRIVER':'dummy', 'SDL_AUDIODRIVER':'dummy'}, check=True)
# Re-run the production overlay path against a complete LVGL build using the
# device's alignment. Keep the two renderer configurations in separate programs.
overlay_arguments = [arg for arg in arguments if not arg.startswith('-DLV_CONF_PATH=')]
overlay_arguments += ['-DLV_CONF_PATH="'+str(root/'firmware/components/pxa_integration/tests/lv_conf_overlay.h')+'"',
    '-I'+str(root/'firmware/components/pxa/tests/esp_stubs'),
    '-I'+str(root/'firmware/components/pxa/include')]
subprocess.run(['cc', *overlay_arguments, '-c',
    str(root/'firmware/components/pxa_integration/tests/application_overlay_test.c'),
    '-o', str(temporary/'overlay.o')], check=True)
overlay_link = [str(temporary/'overlay.o') if arg == str(temporary/'test.o') else
                str(root/'build/tests/lvgl-align64/liblvgl.a') if arg == 'lvgl/liblvgl.a' else arg
                for arg in link]
overlay_link[overlay_link.index('-o')+1] = str(temporary/'overlay')
subprocess.run(overlay_link, cwd=build, check=True)
subprocess.run([str(temporary/'overlay')],
    env={**os.environ, 'SDL_VIDEODRIVER':'dummy', 'SDL_AUDIODRIVER':'dummy'}, check=True)
PY
