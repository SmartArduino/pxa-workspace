#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$(mktemp -d /tmp/pxa-overlay-cache.XXXXXX)"
trap 'rm -rf "$build"' EXIT
lvgl_source="$root/firmware/managed_components/lvgl__lvgl"
lvgl_build="$root/build/tests/lvgl-overlay-cache"
lvgl_library="$lvgl_build/liblvgl.a"
cmake -S "$lvgl_source" -B "$lvgl_build" -DCMAKE_BUILD_TYPE=Release \
    -DLV_BUILD_CONF_PATH="$root/firmware/components/pxa/tests/lv_conf_overlay_cache.h" \
    -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
    -DCONFIG_LV_USE_THORVG=OFF -DCONFIG_LV_USE_SDL=ON \
    -DCONFIG_LV_USE_FREETYPE=ON -DLV_BUILD_INSTALL=OFF >/dev/null
cmake --build "$lvgl_build" --target lvgl -j "${PXA_TEST_JOBS:-6}" >/dev/null
flags=(-std=c11 -O2 -DLV_KCONFIG_IGNORE
    "-DLV_CONF_PATH=\"$root/firmware/components/pxa/tests/lv_conf_overlay_cache.h\""
    -I"$lvgl_source" -I"$lvgl_source/include"
    -I"$root/firmware/components/pxa/tests/esp_stubs"
    -I"$root/firmware/components/pxa/include")
flags+=(-I"$root/deps/pxa-system/libpxa/include")
if [[ -n "${PXA_OVERLAY_TEST_SOURCE:-}" ]]; then
    flags+=("-DPXA_SYSTEM_OVERLAY_SOURCE=\"$PXA_OVERLAY_TEST_SOURCE\"" -I"$root/firmware/components/pxa/src/ui")
fi
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
fi
read -ra libraries <<< "$(pkg-config --libs sdl2 freetype2)"
"${CC:-clang}" "${flags[@]}" "$root/firmware/components/pxa/tests/system_overlay_cache_test.c" \
    "$lvgl_library" "${libraries[@]}" -lm -pthread -o "$build/overlay-cache"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$build/overlay-cache"
