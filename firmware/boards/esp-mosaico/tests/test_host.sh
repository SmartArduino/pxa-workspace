#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/mosaico-tests.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/firmware/components/pxa/include" \
  -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$tests_dir/compositor_test.cc" -o "$build_dir/compositor_test"
"$build_dir/compositor_test"
