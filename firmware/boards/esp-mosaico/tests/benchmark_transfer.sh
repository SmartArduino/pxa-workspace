#!/usr/bin/env bash
set -euo pipefail
tests_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/mosaico-benchmark.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT
"${CXX:-c++}" -std=c++17 -O3 -Wall -Wextra -Werror \
  -I"$tests_dir/../include" "$tests_dir/transfer_benchmark.cc" \
  -o "$build_dir/transfer_benchmark"
"$build_dir/transfer_benchmark"
