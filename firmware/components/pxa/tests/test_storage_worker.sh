#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
out="${PXA_TEST_ARTIFACT_DIR:-$project_dir/local/artifacts/kv-worker/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$out"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g -O1 -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$tests_dir/audio_stubs" -I"$project_dir/deps/pxa-system/libpxa/include" \
  "$tests_dir/pxa_esp_storage_worker_test.cc" -o "$out/kv-worker-test"
"$out/kv-worker-test"
