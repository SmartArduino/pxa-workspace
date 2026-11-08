#!/usr/bin/env bash
set -euo pipefail
component=$(cd "$(dirname "$0")/.." && pwd)
project=$(cd "$component/../../.." && pwd)
output=${PXA_TEST_ARTIFACT_DIR:-"$project/local/artifacts/render-accel-host/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$output"
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-omit-frame-pointer -I"$component/include" \
   "$component/tests/pixels_test.c" "$component/pixels.c" \
   -o "$output/pixels_test"
"$output/pixels_test"
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
   -fno-omit-frame-pointer -DPXA_RASTER_FILL_RGB565=pxa_render_fill_rgb565 \
   -I"$component/include" -I"$project/deps/pxa-system/libpxa/include" \
   "$project/deps/pxa-system/libpxa/tests/raster_test.c" \
   "$project/deps/pxa-system/libpxa/src/services/game_render/raster.c" \
   "$project/deps/pxa-system/libpxa/src/core/wire.c" "$component/pixels.c" \
   -o "$output/raster_accel_test"
"$output/raster_accel_test"
