#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
test_build="$project_dir/build/tests/epdiy-v7"
mkdir -p "$test_build"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" "$test_dir/refresh_test.cc" -o "$test_build/refresh_test"
"$test_build/refresh_test"
for orientation in portrait landscape; do
    mkdir -p "$test_build/$orientation"
    if [[ "$orientation" == portrait ]]; then
        echo '#define CONFIG_PXA_EPDIY_PORTRAIT 1' > "$test_build/$orientation/sdkconfig.h"
    else
        echo '#define CONFIG_PXA_EPDIY_LANDSCAPE 1' > "$test_build/$orientation/sdkconfig.h"
    fi
    "${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$test_build/$orientation" -I"$test_dir/../include" \
        -I"$project_dir/firmware/components/pxa/include" \
        -I"$project_dir/deps/pxa-system/libpxa/include" \
        "$test_dir/surface_test.cc" "$test_dir/../src/epdiy_surface.cc" \
        -o "$test_build/$orientation/surface_test"
    "$test_build/$orientation/surface_test"
done
