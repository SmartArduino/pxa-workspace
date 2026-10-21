#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/pxa-system-clock.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
"${CC:-clang}" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/deps/pxa-system/system/core/include" \
  "$tests_dir/clock_test.c" -o "$build_dir/clock_test"
"$build_dir/clock_test"
