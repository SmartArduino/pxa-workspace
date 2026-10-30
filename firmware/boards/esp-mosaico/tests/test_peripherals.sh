#!/usr/bin/env bash
set -euo pipefail
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$source_dir/../../../.." && pwd)
build_dir=${PXA_TEST_ARTIFACT_DIR:-"$root/local/artifacts/mosaico-peripherals/$(date +%Y%m%d-%H%M%S)/host"}
mkdir -p "$build_dir"
flags=(-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -g)
includes=(-I"$source_dir/peripheral_stubs" -I"$source_dir/../include"
  -I"$root/firmware/components/pxa/include" -I"$root/deps/pxa-system/libpxa/include"
  -I"$root/deps/pxa-system/system/core/include" -I"$root/deps/pxa-system/system/standard/include"
  -I"$root/deps/pxa-system/system/runtimes/native/include"
  -I"$source_dir/../vendor/bmi270" -I"$source_dir/../vendor/bmm150")
for file in bmi270/bmi2 bmi270/bmi270 bmm150/bmm150; do
  "${CC:-cc}" -std=c99 "${flags[@]}" "${includes[@]}" -c "$source_dir/../vendor/$file.c" -o "$build_dir/$(basename "$file").o"
done
"${CC:-cc}" -std=c99 "${flags[@]}" "${includes[@]}" -c \
  "$root/firmware/components/pxa/src/services/pxa_esp_sensor.c" -o "$build_dir/provider.o"
"${CXX:-c++}" -std=c++17 "${flags[@]}" "${includes[@]}" "$source_dir/sensors_test.cc" \
  "$build_dir/bmi2.o" "$build_dir/bmi270.o" "$build_dir/bmm150.o" "$build_dir/provider.o" -o "$build_dir/sensors"
"$build_dir/sensors"
"$build_dir/sensors" missing
"$build_dir/sensors" partial
"$build_dir/sensors" battery
"${CXX:-c++}" -std=c++17 "${flags[@]}" "${includes[@]}" "$source_dir/haptics_test.cc" -o "$build_dir/haptics"
"$build_dir/haptics"
"${CXX:-c++}" -std=c++17 "${flags[@]}" "${includes[@]}" "$source_dir/storage_test.cc" -o "$build_dir/storage"
for stage in {0..21}; do "$build_dir/storage" "$stage"; done
"${CXX:-c++}" -std=c++17 "${flags[@]}" -DCONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED=1 \
  "${includes[@]}" "$source_dir/storage_test.cc" -o "$build_dir/storage-autoformat"
for stage in {0..21} existing missing probe-io probe-internal probe-drive probe-register probe-unmount; do
  "$build_dir/storage-autoformat" "$stage"
done
"${CXX:-c++}" -std=c++17 "${flags[@]}" -I"$source_dir/ports_stubs" \
  -I"$root/extensions/peripherals/include" "${includes[@]}" "$source_dir/ports_test.cc" -o "$build_dir/ports"
"$build_dir/ports"
"$build_dir/ports" v1.0

"$build_dir/ports" retry
"$build_dir/ports" v1.0-retry
"${CXX:-c++}" -std=c++17 "${flags[@]}" -pthread -I"$source_dir/ports_stubs" \
  -I"$root/extensions/peripherals/include" "${includes[@]}" "$source_dir/ports_async_test.cc" -o "$build_dir/ports-async"
"$build_dir/ports-async"
"$build_dir/ports-async" v1.0
