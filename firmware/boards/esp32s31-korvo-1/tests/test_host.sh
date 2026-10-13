#!/usr/bin/env bash
set -euo pipefail
tests_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
test_build=$(mktemp -d "${TMPDIR:-/tmp}/korvo-touch-tests.XXXXXX")
trap 'rm -rf -- "$test_build"' EXIT
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I"$tests_dir/../include" "$tests_dir/touch_state_test.cc" \
    -o "$test_build/touch_state_test"
"$test_build/touch_state_test"
