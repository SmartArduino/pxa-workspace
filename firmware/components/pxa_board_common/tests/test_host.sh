#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
test_build="$(mktemp -d "${TMPDIR:-/tmp}/board-common-tests.XXXXXX")"
trap 'rm -rf -- "$test_build"' EXIT
core="$project_dir/deps/pxa-system/system/core"
"${CC:-cc}" -std=c11 -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
    -fsanitize=address,undefined -I"$core/include" \
    -c "$core/src/display.c" -o "$test_build/display.o"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Wl,--gc-sections \
    -I"$core/include" -I"$tests_dir/../include" \
    "$tests_dir/display_profile_test.cc" "$tests_dir/../src/display_profile.cc" \
    "$test_build/display.o" -o "$test_build/display_profile_test"
"$test_build/display_profile_test"
