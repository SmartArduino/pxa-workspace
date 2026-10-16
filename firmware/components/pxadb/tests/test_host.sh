#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
component_dir="$tests_dir/.."
project_dir="$(cd "$tests_dir/../../../.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/pxadb-jpeg-test.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DCONFIG_PXADB_JPEG_PSRAM_WORKSPACE=1 \
  -DCONFIG_PXADB_ENABLED=1 -DCONFIG_IDF_TARGET_ESP32S3=1 \
  -I"$tests_dir/stubs" -I"$component_dir" -I"$component_dir/include" \
  -I"$project_dir/firmware/managed_components/espressif__esp_new_jpeg/include" \
  "$component_dir/pxadb_jpeg_encode.cc" "$component_dir/pxadb_jpeg_memory.cc" \
  "$tests_dir/jpeg_test.cc" -o "$build_dir/jpeg_test"
"$build_dir/jpeg_test"
