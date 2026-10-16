#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/board-performance.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/deps/pxa-system/system/core/include" \
  -I"$project_dir/deps/pxa-system/system/standard/include" \
  -I"$project_dir/deps/pxa-system/system/runtimes/native/include" \
  -I"$project_dir/deps/pxa-system/ui/reference/lvgl/include" \
  "$tests_dir/performance_test.c" "$tests_dir/../pxa_board_api.c" \
  -o "$build_dir/performance_test"
"$build_dir/performance_test"
