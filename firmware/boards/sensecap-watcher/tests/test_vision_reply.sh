#!/usr/bin/env bash
set -euo pipefail
tests=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$tests/../../../.." && pwd)
out=${PXA_TEST_ARTIFACT_DIR:-"$root/local/artifacts/watcher-vision/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I"$tests/../include" -I"$root/extensions/peripherals/include" \
    -I"$root/deps/pxa-system/libpxa/include" \
    $(pkg-config --cflags libcjson mbedcrypto) \
    "$tests/vision_reply_test.cc" "$tests/../src/watcher_vision_reply.cc" \
    $(pkg-config --libs libcjson mbedcrypto) -o "$out/reply-test"
"$out/reply-test" "$@"
