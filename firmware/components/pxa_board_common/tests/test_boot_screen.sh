#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
lvgl_source="$project_dir/firmware/managed_components/lvgl__lvgl"
lvgl_library="$project_dir/build/tests/lvgl-align64/liblvgl.a"
[[ -f "$lvgl_library" ]] || {
    echo 'Build the real LVGL test library with tools/test-game-system-gestures.sh first.' >&2
    exit 2
}
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/boot-screen-test.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
read -ra libraries <<< "$(pkg-config --libs sdl2 freetype2)"
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DLV_KCONFIG_IGNORE \
    "-DLV_CONF_PATH=\"$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h\"" \
    -I"$test_dir/../include" -I"$lvgl_source" -I"$lvgl_source/include" \
    "$test_dir/boot_screen_test.cc" "$test_dir/../src/boot_screen.cc" \
    "$lvgl_library" "${libraries[@]}" -lm -o "$build_dir/boot_screen_test"
"$build_dir/boot_screen_test"
