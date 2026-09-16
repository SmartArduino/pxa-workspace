#!/usr/bin/env bash
set -euo pipefail

tests_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
component_dir=$(cd "$tests_dir/.." && pwd)
project_dir=$(cd "$component_dir/../../.." && pwd)
pxa_system_dir="$project_dir/deps/pxa-system"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/pxa-esp-host-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

cc_flags=(-std=c99 -Wall -Wextra -Wpedantic -Werror)
adapter_includes=(
  -I"$tests_dir/esp_stubs"
  -I"$component_dir/src/runtime"
  -I"$component_dir/src/services"
  -I"$component_dir/src/package"
  -I"$component_dir/src/ui"
  -I"$component_dir/include"
  -I"$pxa_system_dir/libpxa/include"
  -I"$pxa_system_dir/libpxa/adapters/include"
)

build_and_run() {
  local name=$1
  shift
  cc "${cc_flags[@]}" "$@" -o "$build_dir/$name"
  "$build_dir/$name"
}

build_and_run activation_arena \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_activation_arena_test.c" \
  "$component_dir/src/runtime/pxa_host_activation_arena.c"
build_and_run clock_slots \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_clock_slots_test.c" \
  "$component_dir/src/runtime/pxa_host_clock_slots.c"
build_and_run pointer_mailbox \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_pointer_mailbox_test.c" \
  "$component_dir/src/runtime/pxa_host_pointer_mailbox.c"
build_and_run surface_transform \
  -I"$component_dir/include" \
  "$tests_dir/pxa_surface_transform_test.c"
build_and_run disabled_policy \
  -I"$component_dir/src/package" \
  "$tests_dir/pxa_package_disabled_policy_test.c" \
  "$component_dir/src/package/pxa_package_disabled_policy.c"

libpxa_build="$build_dir/libpxa"
cmake -S "$pxa_system_dir/libpxa" -B "$libpxa_build" \
  -DPXA_BUILD_TESTS=OFF -DPXA_BUILD_ADAPTERS=OFF >/dev/null
cmake --build "$libpxa_build" -j2 >/dev/null

for backend in surface audio net; do
  build_and_run "${backend}_backend" \
    "${adapter_includes[@]}" -pthread \
    "$tests_dir/pxa_esp_${backend}_backend_test.c" \
    "$libpxa_build/libpxa.a" -lm
done

build_and_run lazy_storage \
  "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_lazy_storage_test.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_fs.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_scheduler_store.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_storage.c" \
  "$libpxa_build/libpxa.a" -lm

echo "PXA ESP adapter host tests passed"
