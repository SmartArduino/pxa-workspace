#!/usr/bin/env bash
set -euo pipefail
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$source_dir/../../../.." && pwd)"
output_dir="${PXA_TEST_OUTPUT_DIR:-$project_root/local/artifacts/pai-touch-rotation/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$output_dir"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -I"$project_root/deps/pxa-system/libpxa/include" \
  -I"$project_root/firmware/components/pxa/include" \
  "$source_dir/rotation_test.cc" -o "$output_dir/rotation-test"
"$output_dir/rotation-test"
