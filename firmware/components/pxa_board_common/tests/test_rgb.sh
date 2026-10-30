#!/usr/bin/env bash
set -euo pipefail
tests=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$tests/../../../.." && pwd)
out=${PXA_TEST_ARTIFACT_DIR:-"$root/local/artifacts/board-rgb/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests/rgb_stubs" -I"$tests/../include" \
  -I"$root/firmware/components/pxa/include" \
  -I"$root/extensions/peripherals/include" \
  -I"$root/deps/pxa-system/libpxa/include" \
  "$tests/rgb_port_test.cc" -o "$out/rgb-test"
"$out/rgb-test"
"$out/rgb-test" binary
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests/rgb_stubs" -I"$tests/../include" \
  -I"$root/firmware/components/pxa/include" \
  -I"$root/extensions/peripherals/include" \
  -I"$root/deps/pxa-system/libpxa/include" \
  "$tests/i2c_port_test.cc" -o "$out/i2c-test"
"$out/i2c-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests/../include" -I"$root/firmware/components/pxa/include" \
  -I"$root/extensions/peripherals/include" -I"$root/deps/pxa-system/libpxa/include" \
  "$tests/peripheral_backends_test.cc" -o "$out/providers-test"
"$out/providers-test"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests/rgb_stubs" -I"$tests/../include" \
  -I"$root/firmware/components/pxa/include" \
  -I"$root/extensions/peripherals/include" -I"$root/deps/pxa-system/libpxa/include" \
  "$tests/vision_port_test.cc" -o "$out/vision-test"
"$out/vision-test"
