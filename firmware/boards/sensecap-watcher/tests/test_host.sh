#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/watcher-tests.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -ffunction-sections -Wl,--gc-sections \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/firmware/components/pxa/include" \
  -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$tests_dir/compositor_test.cc" -o "$build_dir/compositor_test"
"$build_dir/compositor_test"
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -ffunction-sections -Wl,--gc-sections \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/firmware/components/pxa/include" \
  -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$tests_dir/lcd_io_test.cc" -o "$build_dir/lcd_io_test"
"$build_dir/lcd_io_test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$tests_dir/../include" "$tests_dir/rtc_calendar_test.cc" -o "$build_dir/rtc_calendar_test"
"$build_dir/rtc_calendar_test"
