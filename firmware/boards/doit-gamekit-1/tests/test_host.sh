#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
test_build="$project_dir/build/tests/doit-gamekit-1"
mkdir -p "$test_build"
for test in input touch wire; do
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" -I"$project_dir/firmware/components/pxa_board_common/include" "$test_dir/${test}_test.cc" -o "$test_build/${test}_test"
"$test_build/${test}_test"
done
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/battery_stubs" -I"$test_dir/../include" \
    "$test_dir/battery_test.cc" "$test_dir/../src/doit_gamekit_1_battery.cc" \
    -o "$test_build/battery_test"
"$test_build/battery_test"
python3 "$project_dir/firmware/components/pxa_integration/tests/test_wifi_preferences.py"
echo "DOIT GameKit 1 input, touch, native portrait wire and battery tests passed"
