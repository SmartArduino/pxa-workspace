#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$tests/../../../.." && pwd)"
system="$root/deps/pxa-system"
core="$system/libpxa"
sdk="$system/sdk/guest-cpp"
out="${PXA_TEST_ARTIFACT_DIR:-$root/local/artifacts/native-raster-demo/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
export TMPDIR="$out"
cflags=(-std=c11 -O2 -ffp-contract=off -I"$core/include")
"${CC:-cc}" "${cflags[@]}" -c "$core/src/services/game_render/raster.c" -o "$out/raster.o"
"${CC:-cc}" "${cflags[@]}" -c "$core/src/core/wire.c" -o "$out/wire.o"
objects=("$out/raster.o" "$out/wire.o")
defines=()
if [[ -n ${PXA_NATIVE_DEMO_BASELINE:-} ]]; then
    git -C "$system" show "${PXA_NATIVE_DEMO_BASELINE}:libpxa/src/services/game_render/raster.c" > "$out/reference.c"
    "${CC:-cc}" "${cflags[@]}" \
        -Dpxa_raster_decode_upload=reference_decode_upload \
        -Dpxa_raster_validate_draw_list=reference_validate_draw_list \
        -Dpxa_raster_execute_draw_list_rows=reference_execute_rows \
        -Dpxa_raster_execute_draw_list=reference_execute_list \
        -c "$out/reference.c" -o "$out/reference.o"
    defines+=(-DPXA_NATIVE_DEMO_REFERENCE)
    objects+=("$out/reference.o")
fi
"${CXX:-c++}" -std=c++2c -O2 -ffp-contract=off -Wall -Wextra -Werror -Wno-attributes \
    -I"$sdk/include" -I"$core/include" "${defines[@]}" \
    "$tests/native_raster_demo_test.cpp" "$sdk/src/game.cpp" "${objects[@]}" -o "$out/demo"
"$out/demo" "$out"
