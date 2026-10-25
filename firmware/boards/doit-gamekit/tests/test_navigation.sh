#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
lvgl_source="$project_dir/firmware/managed_components/lvgl__lvgl"
# Instrument LVGL too: a release archive hides use-after-free inside its
# widget-tree helpers even when the board's test binary enables ASan.
test_output="${PXA_TEST_OUTPUT_ROOT:-$project_dir/local/artifacts/gamekit-navigation/$(date +%Y%m%d-%H%M%S)}"
lvgl_build="$test_output/lvgl-navigation-asan"
cmake -S "$lvgl_source" -B "$lvgl_build" -DCMAKE_BUILD_TYPE=Debug \
    -DLV_BUILD_CONF_PATH="$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h" \
    -DCONFIG_LV_BUILD_DEMOS=OFF -DCONFIG_LV_BUILD_EXAMPLES=OFF \
    -DCONFIG_LV_USE_THORVG=OFF -DCONFIG_LV_USE_SDL=ON \
    -DCONFIG_LV_USE_FREETYPE=ON -DLV_BUILD_INSTALL=OFF \
    '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1' \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -O1' >/dev/null
cmake --build "$lvgl_build" --target lvgl -j "${PXA_TEST_JOBS:-6}" >/dev/null
lvgl_library="$lvgl_build/liblvgl.a"
test_build="$test_output/doit-gamekit"
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
if [[ "$test" == navigation ]]; then
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-deprecated-declarations \
    -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -DLV_KCONFIG_IGNORE \
    -DPXA_TEST_GAMEKIT_1 \
    "-DLV_CONF_PATH=\"$project_dir/firmware/components/pxa_integration/tests/lv_conf_overlay.h\"" \
    -I"$project_dir/firmware/boards/doit-gamekit-1/include" -I"$lvgl_source" -I"$lvgl_source/include" \
    "$test_dir/navigation_test.cc" "$lvgl_library" "${libraries[@]}" -lm \
    -o "$test_build/navigation_gamekit1_test"
"$test_build/navigation_gamekit1_test"
fi
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

# Use the actual firmware dialog constructors with each board's input router.
dialog_flags=("${ime_flags[@]}" -ffunction-sections -fdata-sections
    -I"$project_dir/firmware/components/pxa/tests/esp_stubs"
    -I"$project_dir/firmware/components/pxa/include"
    -I"$project_dir/firmware/components/pxa/src/runtime"
    -I"$project_dir/firmware/components/pxa/src/package"
    -I"$project_dir/deps/pxa-system/libpxa/runtime/include")
"${CC:-clang}" -std=c11 "${dialog_flags[@]}" -c "$test_dir/dialog_fixture.c" -o "$test_build/dialog_fixture.o"
for board in doit-gamekit doit-gamekit-1; do
    board_flags=()
    if [[ "$board" == doit-gamekit-1 ]]; then board_flags+=(-DPXA_TEST_GAMEKIT_1); fi
    "${CXX:-clang++}" -std=c++20 "${dialog_flags[@]}" "${board_flags[@]}" \
        -I"$project_dir/firmware/boards/$board/include" \
        "$test_dir/dialog_navigation_test.cc" "$test_build/dialog_fixture.o" \
        "$lvgl_library" "${libraries[@]}" -Wl,--gc-sections -lm -o "$test_build/dialog_$board"
    "$test_build/dialog_$board"
done
