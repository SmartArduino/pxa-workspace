#!/usr/bin/env bash
# Reproduce the implemented resource path. Firmware builds and the unfinished
# UI/audio integration gates remain separate parts of the overall goal.
set -euo pipefail
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pxa_root="$project_root/deps/pxa-system"
test_root="${PXA_RESOURCE_TEST_ROOT:-$project_root/build/resource-checks}"
app_root="${PXA_APP_SOURCE_ROOT:-$project_root/local/pxa-apps}"
profile="${PXA_RESOURCE_TEST_PROFILE:-pai-touch}"
case "$profile" in pai-touch|sensecap-watcher) ;; *) echo "Unsupported test profile: $profile" >&2; exit 2;; esac
mkdir -p "$test_root/logs"
run_logged() {
    local label=$1
    shift
    printf 'Running %s\n' "$label"
    "$@" >"$test_root/logs/$label.log" 2>&1 || {
        cat "$test_root/logs/$label.log" >&2
        return 1
    }
}
run_logged core-configure cmake -S "$pxa_root/libpxa" -B "$test_root/core" \
    -DPXA_BUILD_TESTS=ON -DPXA_BUILD_ADAPTERS=ON \
    -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
run_logged core-build cmake --build "$test_root/core" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
run_logged core-test ctest --test-dir "$test_root/core" --output-on-failure
run_logged esp-host-test env PXA_TEST_SANITIZERS=1 bash "$project_root/firmware/components/pxa/tests/test_host.sh"
run_logged spec-test python3 "$pxa_root/spec/draft/tools/test.py"
run_logged package-test bash "$pxa_root/tools/package/test_package_tool.sh"
run_logged scenes-generate python3 "$app_root/resource-scenes/generate_resources.py"
run_logged scenes-package env PXA_APP_SOURCE_ROOT="$app_root" \
    PXA_PACKAGE_OUTPUT_ROOT="$test_root/packages" \
    bash "$pxa_root/tools/package/package_app.sh" resource-scenes simulator "$test_root/packages/pxa-resource-scenes"
run_logged pixel-audio-controller-build cc -std=c99 -Wno-attributes \
    -I "$app_root/pixel-dungeon" -I "$pxa_root/sdk/guest-c/include" \
    "$app_root/pixel-dungeon/audio.c" "$app_root/pixel-dungeon/tools/audio_selftest.c" \
    -o "$test_root/pixel-audio-controller-test"
run_logged pixel-audio-controller "$test_root/pixel-audio-controller-test"
run_logged plane-image-controller-build cc -std=c99 -Wno-attributes \
    -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I "$app_root/plane-shooter" -I "$pxa_root/sdk/guest-c/include" \
    "$app_root/plane-shooter/images.c" "$app_root/plane-shooter/tests/images_test.c" \
    -o "$test_root/plane-image-controller-test"
run_logged plane-image-controller "$test_root/plane-image-controller-test"
run_logged store-image-controller-build cc -std=c99 -Wno-attributes \
    -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g \
    -I "$app_root/store" -I "$pxa_root/sdk/guest-c/include" \
    "$app_root/store/search_image.c" "$app_root/store/tests/search_image_test.c" \
    -o "$test_root/store-image-controller-test"
run_logged store-image-controller "$test_root/store-image-controller-test"
run_logged store-image-stop-loading "$test_root/store-image-controller-test" stop-loading
run_logged store-package env PXA_APP_SOURCE_ROOT="$app_root" \
    PXA_PACKAGE_OUTPUT_ROOT="$test_root/packages" \
    bash "$pxa_root/tools/package/package_app.sh" store simulator "$test_root/packages/pxa-store"
run_logged plane-package env PXA_APP_SOURCE_ROOT="$app_root" \
    PXA_PACKAGE_OUTPUT_ROOT="$test_root/packages" \
    bash "$pxa_root/tools/package/package_app.sh" plane-shooter simulator "$test_root/packages/pxa-plane-shooter"
run_logged pixel-package env PXA_APP_SOURCE_ROOT="$app_root" \
    PXA_PACKAGE_OUTPUT_ROOT="$test_root/packages" \
    bash "$pxa_root/tools/package/package_app.sh" pixel-dungeon simulator "$test_root/packages/pxa-pixel-dungeon"
run_logged simulator-configure bash "$project_root/tools/simulator.sh" product configure --profile "$profile"
run_logged simulator-build cmake --build "$project_root/build/simulator/$profile" \
    --target pxsys_resources_test pxsys_resource_budget_test pxsys_audio_test pxsys_audio_app_test pxsys_ui_images_test pxsys_store_images_test -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
# Exercise the production LVGL adapter against real prepared pixel objects.
# The simulator configure step above makes its pinned LVGL source available.
lvgl_test_root="${PXA_LVGL_TEST_ROOT:-$test_root/lvgl}"
run_logged lvgl-configure cmake -S "$pxa_root/libpxa" -B "$lvgl_test_root" \
    -DPXA_BUILD_TESTS=ON -DPXA_BUILD_ADAPTERS=ON \
    -DPXA_LVGL_SOURCE_DIR="$project_root/build/simulator/$profile/_deps/lvgl_source-src" \
    -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
run_logged lvgl-build cmake --build "$lvgl_test_root" --target pxa_lvgl_ui_test -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
run_logged lvgl-images "$lvgl_test_root/pxa_lvgl_ui_test"
run_logged shared-budget "$project_root/build/simulator/$profile/pxsys_resource_budget_test"
run_logged audio-backend python3 "$pxa_root/tools/package/test_audio_backend.py" \
    "$project_root/build/simulator/$profile/pxsys_audio_test" \
    "$pxa_root/simulator/desktop/tests/audio-assets"
runner="$project_root/build/simulator/$profile/pxsys_resources_test"
package="$test_root/packages/pxa-resource-scenes"
key="${PXA_SIMULATOR_PUBLISHER_KEY:-$app_root/.dev-signing/publisher-public.der}"
# Fixed budgets make the default/pressure comparison independent of the shell.
export PXA_RESOURCE_INTERNAL_BYTES=131072 PXA_RESOURCE_EXTERNAL_BYTES=2097152
export PXA_RESOURCE_TEMPORARY_INTERNAL_BYTES=16384 PXA_RESOURCE_TEMPORARY_EXTERNAL_BYTES=524288
export PXA_ASSET_READ_DELAY_US=1000
export PXA_STORAGE_BYTES_PER_SECOND=0 PXA_STORAGE_LATENCY_US=0
image_runner="$project_root/build/simulator/$profile/pxsys_ui_images_test"
image_package="$test_root/packages/pxa-plane-shooter"
mkdir -p "$test_root/plane-opening" "$test_root/plane-walk" "$test_root/plane-low" \
    "$test_root/plane-background" "$test_root/plane-exit"
run_logged plane-opening "$image_runner" "$image_package" "$key" "$test_root/plane-opening" wait-ready
run_logged plane-walk "$image_runner" "$image_package" "$key" "$test_root/plane-walk" walk
run_logged plane-low env PXA_RESOURCE_EXTERNAL_BYTES=180000 "$image_runner" \
    "$image_package" "$key" "$test_root/plane-low" walk
run_logged plane-background env PXA_ASSET_READ_DELAY_US=20000 "$image_runner" \
    "$image_package" "$key" "$test_root/plane-background" background-loading
run_logged plane-exit env PXA_ASSET_READ_DELAY_US=20000 "$image_runner" \
    "$image_package" "$key" "$test_root/plane-exit" exit-loading
store_runner="$project_root/build/simulator/$profile/pxsys_store_images_test"
store_package="$test_root/packages/pxa-store"
mkdir -p "$test_root/store-ready" "$test_root/store-background" "$test_root/store-exit" "$test_root/store-low"
run_logged store-ready "$store_runner" "$store_package" "$key" "$test_root/store-ready" ready
run_logged store-background env PXA_ASSET_READ_DELAY_US=100000 "$store_runner" \
    "$store_package" "$key" "$test_root/store-background" background-loading
run_logged store-exit env PXA_ASSET_READ_DELAY_US=100000 "$store_runner" \
    "$store_package" "$key" "$test_root/store-exit" exit-loading
run_logged store-low env PXA_ASSET_EXTERNAL_BYTES=4096 "$store_runner" \
    "$store_package" "$key" "$test_root/store-low" low-budget
run_logged pixel-audio-app "$project_root/build/simulator/$profile/pxsys_audio_app_test" \
    "$test_root/packages/pxa-pixel-dungeon" "$key"
run_logged scenes-default "$runner" "$package" "$key"
run_logged scenes-low-budget env PXA_RESOURCE_EXTERNAL_BYTES=300000 "$runner" "$package" "$key"
run_logged storage-steady "$runner" "$package" "$key" storage-steady
run_logged storage-stall "$runner" "$package" "$key" storage-stall
run_logged storage-stall-low-budget env PXA_RESOURCE_EXTERNAL_BYTES=300000 "$runner" "$package" "$key" storage-stall
run_logged storage-exit "$runner" "$package" "$key" storage-exit
run_logged exit-reading env PXA_ASSET_READ_DELAY_US=20000 "$runner" "$package" "$key" exit-reading
run_logged exit-loading env PXA_ASSET_READ_DELAY_US=20000 "$runner" "$package" "$key" exit-loading
run_logged exit-sound-loading env PXA_ASSET_READ_DELAY_US=20000 "$runner" "$package" "$key" exit-sound-loading
printf 'Resource path checks passed. Logs: %s/logs\n' "$test_root"
