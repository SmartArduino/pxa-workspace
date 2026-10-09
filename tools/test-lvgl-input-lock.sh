#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d /tmp/pxa-lvgl-input.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
lvgl_source="$root/firmware/managed_components/lvgl__lvgl"
lvgl_library="$root/build/simulator/pai-touch/lvgl/liblvgl.a"
[[ -f "$lvgl_library" ]] || { echo 'Build the pai-touch simulator first.' >&2; exit 2; }
flags=(-std=c11 -O2 -pthread -D_POSIX_C_SOURCE=200809L -DLV_KCONFIG_IGNORE
    "-DLV_CONF_PATH=\"$root/deps/pxa-system/simulator/desktop/lv_conf.h\""
    -I"$lvgl_source" -I"$lvgl_source/include")
read -ra libraries <<< "$(pkg-config --libs sdl2 freetype2)"
"${CC:-clang}" "${flags[@]}" \
    "$root/firmware/components/pxa_lvgl_input/input_lock_test.c" \
    "$root/firmware/components/pxa_lvgl_input/lvgl_serialized_input.c" \
    "$lvgl_library" "${libraries[@]}" -lm -Wl,--wrap=lv_indev_read \
    -o "$test_dir/input-lock"
"$test_dir/input-lock"
