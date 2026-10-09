#!/usr/bin/env bash
set -euo pipefail

tests_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
component_dir=$(cd "$tests_dir/.." && pwd)
project_dir=$(cd "$component_dir/../../.." && pwd)
pxa_system_dir="$project_dir/deps/pxa-system"
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/pxa-esp-host-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

cc_flags=(-std=c99 -Wall -Wextra -Wpedantic -Werror)
cxx_flags=(-std=c++17 -Wall -Wextra -Werror)
libpxa_options=()
if [[ ${PXA_TEST_SANITIZERS:-0} == 1 ]]; then
  sanitizer_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  cc_flags+=("${sanitizer_flags[@]}")
  cxx_flags+=("${sanitizer_flags[@]}")
  libpxa_options+=("-DCMAKE_C_FLAGS=${sanitizer_flags[*]}")
fi
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
build_and_run store_policy \
  -I"$component_dir/src/runtime" \
  -I"$component_dir/src/package" \
  -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_esp_store_policy_test.c"
build_and_run install_space \
  -I"$pxa_system_dir/libpxa/include" \
  -I"$pxa_system_dir/libpxa/adapters/include" \
  "$tests_dir/pxa_install_space_test.c"
build_and_run surface_transform \
  -I"$component_dir/include" \
  "$tests_dir/pxa_surface_transform_test.c"
build_and_run disabled_policy \
  -I"$component_dir/src/package" \
  "$tests_dir/pxa_package_disabled_policy_test.c" \
  "$component_dir/src/package/pxa_package_disabled_policy.c"
build_and_run wasi_stdio -DESP_PLATFORM=1 \
  "${adapter_includes[@]}" "$tests_dir/pxa_esp_wasi_stdio_test.c"

libpxa_build="$build_dir/libpxa"
cmake -S "$pxa_system_dir/libpxa" -B "$libpxa_build" \
  -DPXA_BUILD_TESTS=OFF -DPXA_BUILD_ADAPTERS=OFF "${libpxa_options[@]}" >/dev/null
cmake --build "$libpxa_build" -j2 >/dev/null

cc "${cc_flags[@]}" "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_assets_backend_test.c" "$libpxa_build/libpxa.a" \
  -lmbedcrypto -lm -o "$build_dir/assets_backend"
python3 "$pxa_system_dir/tools/package/test_resource_worker.py" "$build_dir/assets_backend" --music

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

c++ "${cxx_flags[@]}" -DESP_PLATFORM=1 \
  -I"$tests_dir/audio_stubs" -I"$component_dir/include" -I"$component_dir/src/services" \
  -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_audio_output_test.cc" "$component_dir/src/services/pxa_codec_memory.cc" \
  "$libpxa_build/libpxa.a" -o "$build_dir/audio_output"
"$build_dir/audio_output"

c++ "${cxx_flags[@]}" -pthread \
  -I"$component_dir/src/services" -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_codec_memory_test.cc" "$component_dir/src/services/pxa_codec_memory.cc" \
  "$libpxa_build/libpxa.a" -o "$build_dir/codec_memory"
"$build_dir/codec_memory"
python3 "$project_dir/tools/test-audit-codec-memory.py"

echo "PXA ESP adapter host tests passed"
