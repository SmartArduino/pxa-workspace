#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir=${PXA_TEST_ARTIFACT_DIR:-"$project_dir/local/artifacts/mosaico-host/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$build_dir"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I"$tests_dir/stubs" -I"$tests_dir/../include" \
  -I"$project_dir/firmware/components/pxa/include" \
  -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$tests_dir/compositor_test.cc" -o "$build_dir/compositor_test"
"$build_dir/compositor_test"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -pthread \
  -I"$tests_dir/audio_stubs" \
  -I"$project_dir/firmware/components/pxa/tests/audio_stubs" \
  -I"$tests_dir/../include" \
  "$tests_dir/audio_test.cc" "$tests_dir/../src/esp_mosaico_audio.cc" \
  -o "$build_dir/audio_test"
"$build_dir/audio_test"

bash "$tests_dir/test_peripherals.sh"

bash "$project_dir/firmware/components/pxa/tests/test_audio_capture_input.sh"
