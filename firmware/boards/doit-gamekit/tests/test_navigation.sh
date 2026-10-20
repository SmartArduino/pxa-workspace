#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
lvgl_source="$project_dir/firmware/managed_components/lvgl__lvgl"
# Instrument LVGL too: a release archive hides use-after-free inside its
# widget-tree helpers even when the board's test binary enables ASan.
lvgl_build="$project_dir/build/tests/lvgl-navigation-asan"
cmake -S "$lvgl_source" -B "$lvgl_build" -DCMAKE_BUILD_TYPE=Debug \
    -DLV_BUILD_CONF_PATH="$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h" \
    -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
    -DCONFIG_LV_USE_THORVG=OFF -DCONFIG_LV_USE_SDL=ON \
    -DCONFIG_LV_USE_FREETYPE=ON -DLV_BUILD_INSTALL=OFF \
    '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1' \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1' >/dev/null
cmake --build "$lvgl_build" --target lvgl -j "${PXA_TEST_JOBS:-6}" >/dev/null
lvgl_library="$lvgl_build/liblvgl.a"
test_build="$project_dir/build/tests/doit-gamekit"
mkdir -p "$test_build"
read -ra libraries <<< "$(pkg-config --libs sdl2 freetype2)"
for test in navigation refresh; do
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DLV_KCONFIG_IGNORE \
    "-DLV_CONF_PATH=\"$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h\"" \
    -I"$test_dir/../include" -I"$lvgl_source" -I"$lvgl_source/include" \
    "$test_dir/${test}_test.cc" "$lvgl_library" "${libraries[@]}" -lm \
    -o "$test_build/${test}_test"
"$test_build/${test}_test"
done

# Compile the real shared IME frontend with sanitizers too. The local decoder
# and PXA Provider path must both work with this exact board keypad adapter.
ime_source="$project_dir/deps/pxa-system/libpxa/adapters/lvgl"
ime_flags=(-O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DLV_KCONFIG_IGNORE
    "-DLV_CONF_PATH=\"$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h\""
    -I"$lvgl_source" -I"$lvgl_source/include"
    -I"$project_dir/deps/pxa-system/libpxa/adapters/include"
    -I"$project_dir/deps/pxa-system/libpxa/include")
for source in pxa_lvgl_ime pxa_lvgl_ime_builtin; do
    "${CC:-clang}" -std=c11 "${ime_flags[@]}" -c "$ime_source/$source.c" -o "$test_build/$source.o"
done
"${CXX:-clang++}" -std=c++20 "${ime_flags[@]}" -I"$test_dir/../include" \
    "$test_dir/ime_navigation_test.cc" "$test_build/pxa_lvgl_ime.o" "$test_build/pxa_lvgl_ime_builtin.o" \
    "$lvgl_library" "${libraries[@]}" -lm -o "$test_build/ime_navigation_test"
"$test_build/ime_navigation_test"
