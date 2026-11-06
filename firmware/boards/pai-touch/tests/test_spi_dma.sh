#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
"${CXX:-clang++}" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$source_dir/spi_stubs" -I"$source_dir/../include" \
  "$source_dir/../src/pai_touch_display_dma.cc" "$source_dir/spi_dma_test.cc" -o "$binary"
"$binary"
