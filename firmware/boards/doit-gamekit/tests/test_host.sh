#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
test_build="$project_dir/build/tests/doit-gamekit"
mkdir -p "$test_build"
for test in input surface rotation; do
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$test_dir/../include" -I"$project_dir/firmware/components/pxa_board_common/include" -I"$project_dir/firmware/components/pxa/include" \
  -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$test_dir/${test}_test.cc" -o "$test_build/${test}_test"
"$test_build/${test}_test"
done
echo "DOIT GameKit input, compositor and rotation tests passed"
