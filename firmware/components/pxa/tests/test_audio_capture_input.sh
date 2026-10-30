#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$source_dir/../../../.." && pwd)
build_dir=${PXA_TEST_ARTIFACT_DIR:-"$root/local/artifacts/audio-capture/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -pthread \
  -I"$source_dir/capture_stubs" -I"$source_dir/audio_stubs" \
  -I"$source_dir/../include" -I"$source_dir/../src/services" \
  -I"$root/deps/pxa-system/libpxa/include" \
  "$source_dir/pxa_audio_capture_input_test.cc" "$source_dir/../src/services/pxa_audio_capture_input.cc" \
  -o "$build_dir/capture-input"
for stage in 1 2 3; do "$build_dir/capture-input" "$stage"; done
"$build_dir/capture-input"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g -pthread \
  -I"$source_dir/audio_stubs" -I"$source_dir/../include" \
  "$source_dir/pxa_worker_notifications_test.cc" -o "$build_dir/worker-notifications"
"$build_dir/worker-notifications"
