#!/usr/bin/env bash
set -euo pipefail
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$test_dir/../../../.." && pwd)"
test_build="${PXA_TEST_OUTPUT:-$project_dir/local/artifacts/espp4-a/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$test_build"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" -I"$project_dir/firmware/components/pxa/include" \
    -I"$project_dir/firmware/components/pxa_board_common/include" \
    -I"$project_dir/firmware/components/pxa_render_accel/include" \
    -I"$project_dir/deps/pxa-system/libpxa/include" \
    "$test_dir/surface_test.cc" "$test_dir/../src/espp4_a_surface.cc" \
    "$test_dir/../src/espp4_a_pixels.cc" "$project_dir/firmware/components/pxa_render_accel/pixels.c" \
    -o "$test_build/surface_test"
"$test_build/surface_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" -I"$project_dir/firmware/components/pxa_render_accel/include" \
    "$test_dir/pixels_test.cc" "$test_dir/../src/espp4_a_pixels.cc" \
    "$project_dir/firmware/components/pxa_render_accel/pixels.c" -o "$test_build/pixels_test"
"$test_build/pixels_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" -I"$project_dir/firmware/components/pxa_board_common/include" \
    "$test_dir/scanout_test.cc" -o "$test_build/scanout_test"
"$test_build/scanout_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/rotation_stubs" -I"$test_dir/../include" \
    -I"$project_dir/deps/pxa-system/libpxa/include" \
    "$test_dir/rotation_test.cc" "$test_dir/../src/espp4_a_rotation.cc" \
    -o "$test_build/rotation_test"
"$test_build/rotation_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" -I"$project_dir/firmware/components/pxadb/include" \
    "$test_dir/touch_test.cc" -o "$test_build/touch_test"
"$test_build/touch_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -pthread -I"$test_dir/../include" -I"$test_dir/sdio_stubs" "$test_dir/sdio_test.cc" \
    "$test_dir/../src/espp4_a_sdio.cc" -o "$test_build/sdio_test"
"$test_build/sdio_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/sdio_stubs" -I"$test_dir/../include" \
    "$test_dir/sd_test.cc" "$test_dir/../src/espp4_a_sd.cc" -o "$test_build/sd_test"
"$test_build/sd_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" "$test_dir/camera_buffers_test.cc" \
    -o "$test_build/camera_buffers_test"
"$test_build/camera_buffers_test"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" "$test_dir/camera_geometry_test.cc" \
    -o "$test_build/camera_geometry_test"
"$test_build/camera_geometry_test"

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$test_dir/../include" "$test_dir/usb_download_trigger_test.cc" \
    -o "$test_build/usb_download_trigger_test"
"$test_build/usb_download_trigger_test"
python3 "$test_dir/test_usb_restart.py"
python3 "$test_dir/test_usb_esptool.py"
