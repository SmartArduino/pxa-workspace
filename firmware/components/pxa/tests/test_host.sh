#!/usr/bin/env bash
set -euo pipefail

tests_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
component_dir=$(cd "$tests_dir/.." && pwd)
project_dir=$(cd "$component_dir/../../.." && pwd)
pxa_system_dir=${PXSYS_ROOT:-"$project_dir/deps/pxa-system"}
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/pxa-esp-host-tests.XXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

python3 "$project_dir/firmware/components/pxa_integration/tests/test_startup.py"
python3 "$project_dir/firmware/components/pxa_integration/tests/test_storage_startup.py"
python3 "$tests_dir/test_native_move_ui.py"
python3 "$tests_dir/test_native_file_ui.py"
python3 "$tests_dir/test_legacy_catalog.py"
python3 "$tests_dir/test_graceful_stop.py"
python3 "$tests_dir/test_stop_cleanup.py"

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

build_and_run ui_event_text \
  "${adapter_includes[@]}" "$tests_dir/pxa_host_ui_event_text_test.c"
build_and_run maintenance \
  -I"$component_dir/src/runtime" "$tests_dir/pxa_host_maintenance_test.c"
build_and_run main_loop \
  "${adapter_includes[@]}" "$tests_dir/pxa_host_main_loop_test.c"
build_and_run window_metrics \
  "${adapter_includes[@]}" "$tests_dir/pxa_host_window_metrics_test.c"

build_and_run activation_arena \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_activation_arena_test.c" \
  "$component_dir/src/runtime/pxa_host_activation_arena.c"
build_and_run clock_slots \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_clock_slots_test.c" \
  "$component_dir/src/runtime/pxa_host_clock_slots.c"
build_and_run raster_handoff \
  -I"$component_dir/src/services" -pthread "$tests_dir/pxa_raster_handoff_test.c"
build_and_run controller_state \
  -I"$component_dir/src/runtime" "$tests_dir/pxa_host_controller_state_test.c"
build_and_run pointer_mailbox \
  -I"$component_dir/src/runtime" \
  "$tests_dir/pxa_host_pointer_mailbox_test.c" \
  "$component_dir/src/runtime/pxa_host_pointer_mailbox.c"
build_and_run identity_match \
  -I"$component_dir/src/package" "$tests_dir/pxa_package_identity_match_test.c"
build_and_run app_locators \
  -I"$component_dir/src/package" -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_app_locators_test.c" "$component_dir/src/package/pxa_app_locators.c"
build_and_run private_data_move \
  -I"$component_dir/src/package" -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_private_data_move_test.c"
build_and_run locator_migration \
  -I"$component_dir/src/package" -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_locator_migration_test.c" "$component_dir/src/package/pxa_locator_migration.c" \
  "$component_dir/src/package/pxa_app_locators.c"
if pkg-config --exists libcjson; then
  build_and_run publisher_profile \
    -I"$component_dir/src/package" -I"$pxa_system_dir/libpxa/include" \
    $(pkg-config --cflags libcjson) "$tests_dir/pxa_publisher_profile_test.c" \
    "$component_dir/src/package/pxa_publisher_profile.c" $(pkg-config --libs libcjson)
fi
build_and_run store_policy \
  -I"$component_dir/src/runtime" \
  -I"$component_dir/src/package" \
  -I"$pxa_system_dir/libpxa/include" \
  -I"$pxa_system_dir/libpxa/adapters/include" \
  "$tests_dir/pxa_esp_store_policy_test.c"
build_and_run storage_path_policy \
    "$tests_dir/pxa_esp_storage_path_policy_test.c"
build_and_run legacy_permissions -DESP_PLATFORM=1 \
  "-DCONFIG_PXA_MOUNT_POINT=\"$build_dir/legacy-permissions\"" \
  '-DCONFIG_PXA_STATE_ROOT="pxa-state"' \
  "${adapter_includes[@]}" "$tests_dir/pxa_esp_legacy_permission_test.c" -lmbedcrypto -pthread
build_and_run media_path \
    "$tests_dir/pxa_esp_media_path_test.c"
build_and_run install_space \
  -I"$pxa_system_dir/libpxa/include" \
  -I"$pxa_system_dir/libpxa/adapters/include" \
  "$tests_dir/pxa_install_space_test.c"
build_and_run store_download -DESP_PLATFORM=1 \
  "-DCONFIG_PXA_MOUNT_POINT=\"$build_dir/store-download\"" \
  "${adapter_includes[@]}" "$tests_dir/pxa_esp_store_download_test.c" \
  "$pxa_system_dir/libpxa/src/package/container.c" "$pxa_system_dir/libpxa/src/core/wire.c" -lmbedcrypto
build_and_run store_cache -DESP_PLATFORM=1 \
  "${adapter_includes[@]}" "$tests_dir/pxa_esp_store_cache_test.c" \
  "$component_dir/src/package/pxa_esp_store_cache.c" \
  "$pxa_system_dir/libpxa/src/core/wire.c" -lcrypto -lmbedcrypto
build_and_run surface_transform \
  -I"$component_dir/include" \
  "$tests_dir/pxa_surface_transform_test.c"
build_and_run disabled_policy \
  -I"$component_dir/src/package" \
  "$tests_dir/pxa_package_disabled_policy_test.c" \
  "$component_dir/src/package/pxa_package_disabled_policy.c"
build_and_run wasi_stdio -DESP_PLATFORM=1 \
  "${adapter_includes[@]}" "$tests_dir/pxa_esp_wasi_stdio_test.c"
build_and_run early_security "${adapter_includes[@]}" "$tests_dir/pxa_esp_security_test.c"
build_and_run directory_handles -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  "${adapter_includes[@]}" -pthread "$tests_dir/pxa_esp_directory_test.c"
build_and_run native_fs_scan -DESP_PLATFORM=1 -DPXA_POSIX_NATIVE_DIR_SCAN=1 \
  -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_fs_scan_test.c" "$pxa_system_dir/libpxa/adapters/tests/fs_tree_test.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_fs.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" -Wl,--wrap=readdir
PXA_TEST_UNKNOWN_DIRENT=1 "$build_dir/native_fs_scan"
littlefs_dir="$project_dir/firmware/managed_components/joltwallet__littlefs/src/littlefs"
build_and_run private_image -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  "${adapter_includes[@]}" -I"$littlefs_dir" -pthread \
  "$tests_dir/pxa_esp_image_blockdev_test.c" "$component_dir/src/services/pxa_esp_image_blockdev.c" \
  "$littlefs_dir/lfs.c" "$littlefs_dir/lfs_util.c"

libpxa_build="$build_dir/libpxa"
cmake -S "$pxa_system_dir/libpxa" -B "$libpxa_build" \
  -DPXA_BUILD_TESTS=OFF -DPXA_BUILD_ADAPTERS=OFF "${libpxa_options[@]}" >/dev/null
cmake --build "$libpxa_build" -j2 >/dev/null
python3 "$tests_dir/test_font_source.py" --libpxa "$libpxa_build/libpxa.a"
build_and_run install_approval "${adapter_includes[@]}" \
  "$tests_dir/pxa_esp_install_approval_test.c" "$component_dir/src/package/pxa_esp_install_approval.c" \
  "$libpxa_build/libpxa.a" -lm
build_and_run private_copy "${adapter_includes[@]}" \
  "$tests_dir/pxa_esp_private_copy_test.c" "$component_dir/src/services/pxa_esp_private_copy.c" \
  "$pxa_system_dir/libpxa/adapters/openssl/pxa_openssl.c" "$libpxa_build/libpxa.a" -lcrypto -lm
build_and_run transfer_receipt "${adapter_includes[@]}" -I"$pxa_system_dir/libpxa/adapters/tests" \
  "$tests_dir/pxa_esp_transfer_receipt_test.c" "$libpxa_build/libpxa.a" -lcrypto -lm
build_and_run record_seal "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_record_seal_test.c" "$libpxa_build/libpxa.a" -lmbedcrypto -lm
build_and_run package_trust -DESP_PLATFORM=1 -DCONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY=1 \
  "${adapter_includes[@]}" -pthread "$tests_dir/pxa_esp_package_trust_test.c" \
  "$libpxa_build/libpxa.a" -lm
build_and_run volumes -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  "${adapter_includes[@]}" -pthread "$tests_dir/pxa_esp_volumes_test.c" \
  "$component_dir/src/services/pxa_esp_volumes.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_volume.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_store.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_install_record.c" \
  "$libpxa_build/libpxa.a" -lmbedcrypto -lm
build_and_run fs_roots -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  "-DCONFIG_PXA_MOUNT_POINT=\"$build_dir/scoped\"" '-DCONFIG_PXA_STATE_ROOT="state"' \
  "${adapter_includes[@]}" -pthread "$tests_dir/pxa_esp_fs_roots_test.c" \
  "$component_dir/src/services/pxa_esp_volumes.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_volume.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_scoped_fs.c" \
  "$libpxa_build/libpxa.a" -lcrypto -lmbedcrypto -lm
build_and_run app_storage -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR \
  "${adapter_includes[@]}" -I"$littlefs_dir" -I"$pxa_system_dir/libpxa/adapters/tests" -pthread \
  "$tests_dir/pxa_esp_app_storage_test.c" "$component_dir/src/services/pxa_esp_app_storage.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_home.c" \
  "$component_dir/src/services/pxa_esp_image_blockdev.c" "$component_dir/src/services/pxa_esp_volumes.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_volume.c" \
  "$littlefs_dir/lfs.c" "$littlefs_dir/lfs_util.c" "$libpxa_build/libpxa.a" -lcrypto -lmbedcrypto -lm
cc "${cc_flags[@]}" -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  '-DCONFIG_PXA_STORAGE_PARTITION_LABEL="forbidden-primary"' \
  "${adapter_includes[@]}" -I"$pxa_system_dir/libpxa/adapters/tests" -pthread \
  "$tests_dir/pxa_esp_installations_test.c" "$component_dir/src/package/pxa_esp_installations.c" \
  "$component_dir/src/package/pxa_esp_resource_transfer.c" \
  "$component_dir/src/services/pxa_esp_volumes.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_volume.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_installer.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_fs.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_store.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_home.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_install_record.c" \
  "$pxa_system_dir/libpxa/adapters/openssl/pxa_openssl.c" \
  "$libpxa_build/libpxa.a" -llz4 -lcrypto -lmbedcrypto -lm -o "$build_dir/installations"
python3 "$tests_dir/test_installations.py" "$build_dir/installations" "$pxa_system_dir"

for migration in app_transfer legacy_adoption; do
adoption_sources=()
if [[ $migration == legacy_adoption ]]; then
  adoption_sources+=("$component_dir/src/package/pxa_esp_legacy_adoption.c"
    "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_storage.c")
fi
cc "${cc_flags[@]}" -DESP_PLATFORM=1 -DCONFIG_LITTLEFS_FCNTL_GET_PATH=0 \
  '-DCONFIG_PXA_STORAGE_PARTITION_LABEL="forbidden-primary"' \
  "${adapter_includes[@]}" -I"$pxa_system_dir/libpxa/adapters/tests" -pthread \
  "$tests_dir/pxa_esp_${migration}_test.c" "${adoption_sources[@]}" "$component_dir/src/package/pxa_esp_installations.c" \
  "$component_dir/src/services/pxa_esp_volumes.c" \
  "$component_dir/src/services/pxa_esp_app_storage.c" \
  "$component_dir/src/services/pxa_esp_image_blockdev.c" \
  "$component_dir/src/services/pxa_esp_private_copy.c" \
  "$component_dir/src/services/pxa_esp_private_authority.c" \
  "$component_dir/src/package/pxa_esp_transfer_receipt.c" \
  "$pxa_system_dir/libpxa/adapters/esp/pxa_esp_posix_shim.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_volume.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_installer.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_fs.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_store.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_app_home.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_install_record.c" \
  "$pxa_system_dir/libpxa/adapters/openssl/pxa_openssl.c" \
  "$libpxa_build/libpxa.a" -llz4 -lcrypto -lmbedcrypto -lm -o "$build_dir/$migration"
python3 "$tests_dir/test_installations.py" "$build_dir/$migration" "$pxa_system_dir"
done

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

# Constrained-board policy must retain budget charging and release semantics
# while placing large raster assets directly in PSRAM.
build_and_run surface_external_palette \
  -DCONFIG_PXA_RASTER_INTERNAL_ALLOCATION_MAX_BYTES=4096 \
  "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_surface_backend_test.c" \
  "$libpxa_build/libpxa.a" -lm

build_and_run lazy_storage \
  "${adapter_includes[@]}" -pthread \
  "$tests_dir/pxa_esp_lazy_storage_test.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_fs.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_scheduler_store.c" \
  "$pxa_system_dir/libpxa/adapters/posix/pxa_posix_storage.c" \
  "$libpxa_build/libpxa.a" -lm

for affinity in -1 0 1; do
  c++ "${cxx_flags[@]}" -DESP_PLATFORM=1 \
    -DCONFIG_PXA_AUDIO_OUTPUT_TASK_AFFINITY="$affinity" \
    -I"$tests_dir/audio_stubs" -I"$component_dir/include" -I"$component_dir/src/services" \
    -I"$pxa_system_dir/libpxa/include" \
    "$tests_dir/pxa_audio_output_test.cc" "$component_dir/src/services/pxa_codec_memory.cc" \
    "$libpxa_build/libpxa.a" -o "$build_dir/audio_output"
  "$build_dir/audio_output"
done

c++ "${cxx_flags[@]}" -pthread \
  -I"$component_dir/src/services" -I"$pxa_system_dir/libpxa/include" \
  "$tests_dir/pxa_codec_memory_test.cc" "$component_dir/src/services/pxa_codec_memory.cc" \
  "$libpxa_build/libpxa.a" -o "$build_dir/codec_memory"
"$build_dir/codec_memory"
python3 "$project_dir/tools/test-audit-codec-memory.py"

echo "PXA ESP adapter host tests passed"
