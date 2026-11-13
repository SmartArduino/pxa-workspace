#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$test_dir/../../../.." && pwd)"
test_build="$(mktemp -d /tmp/pxa-p4-ui-render.XXXXXX)"
trap 'rm -rf "$test_build"' EXIT
lvgl_source="$root/firmware/managed_components/lvgl__lvgl"
lvgl_build="$root/build/tests/lvgl-overlay-cache"
cmake -S "$lvgl_source" -B "$lvgl_build" -DCMAKE_BUILD_TYPE=Release \
    -DLV_BUILD_CONF_PATH="$root/firmware/components/pxa/tests/lv_conf_overlay_cache.h" \
    -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
    -DCONFIG_LV_USE_THORVG=OFF -DCONFIG_LV_USE_SDL=ON \
    -DCONFIG_LV_USE_FREETYPE=ON -DLV_BUILD_INSTALL=OFF >/dev/null
cmake --build "$lvgl_build" --target lvgl -j "${PXA_TEST_JOBS:-6}" >/dev/null
read -ra libraries <<< "$(pkg-config --libs sdl2 freetype2)"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -DLV_KCONFIG_IGNORE \
    "-DLV_CONF_PATH=\"$root/firmware/components/pxa/tests/lv_conf_overlay_cache.h\"" \
    -I"$lvgl_source" -I"$lvgl_source/include" -I"$test_dir/../include" \
    "$test_dir/ui_render_test.cc" "$lvgl_build/liblvgl.a" \
    "${libraries[@]}" -lm -pthread -o "$test_build/ui-render"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$test_build/ui-render"
