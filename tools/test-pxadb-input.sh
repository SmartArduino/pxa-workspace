#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_dir="$(mktemp -d /tmp/pxa-pxadb-input.XXXXXX)"
trap 'rm -rf "$test_dir"' EXIT
"${CXX:-clang++}" -std=c++2c -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$root/firmware/components/pxadb/input_mailbox_test.cpp" -o "$test_dir/input"
"$test_dir/input"
"${CXX:-clang++}" -std=c++2c -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/firmware/components/pxadb/include" \
    "$root/firmware/components/pxadb/fs_policy_test.cpp" -o "$test_dir/fs-policy"
"$test_dir/fs-policy"
echo 'PXADB multitouch state and mailbox ASan/UBSan tests OK'
echo 'PXADB protected parent creation policy ASan/UBSan tests OK'
