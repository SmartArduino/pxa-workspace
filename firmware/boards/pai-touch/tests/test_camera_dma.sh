#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$source_dir/camera_stubs" \
    "$source_dir/../src/pai_touch_camera_dma.c" "$source_dir/camera_dma_test.c" -o "$binary"
"$binary"
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$source_dir/../include" \
    "$source_dir/camera_pixels_test.cpp" -o "$binary"
"$binary"
