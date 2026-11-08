# PXA ESP32-S31 rendering acceleration

This component supplies pixel operations to PXA GameRender, LVGL and board
presenters. `CONFIG_PXA_RENDER_ACCEL` defaults on for ESP32-S31. Other targets
retain the portable implementation. The application and GameRender wire ABI do
not change.

## Operations

* Core 1 PIE/SIMD: RGB565 fills, constant-color opacity blends, byte swaps, exact nearest-neighbor 2x/4x row
  expansion and equal-prefix comparison. Call from task context. SIMD requires
  a task pinned to core 1; core 0, unpinned tasks and unaligned/small requests
  use scalar code without changing task affinity. Vector loops neither read
  nor write beyond the requested pixels. Byte swap permits an identical source
  and destination; other overlap is unsupported.
* PPA: full-width background bands and leading GameRender color clears. CPU
  workers still reset depth/coverage and execute subsequent drawing commands.
  Requests below 65,536 pixels and buffers with shared boundary cache lines
  fall back to CPU rendering. Both pixel and byte stride must be respected by
  callers.
* GameRender's color-fill primitive also uses SIMD for eligible solid spans,
  rectangles and CPU fallback clears. The same backend is selected by all S31
  board builds; board presenters can reuse the public pixel operations.
* LVGL: the existing custom software-blend hook accelerates opaque RGB565
  fills and unmasked constant-color opacity blends on core 1. Blending retains
  LVGL's exact five-bit opacity quantization. The SIMD kernel saves/restores
  SAR and uses exact intermediate products without changing rounding mode.
  Masks, small fills and other formats retain LVGL's
  renderer. This does not enable LVGL's per-primitive PPA draw unit.
* INDEX8 palette resolve has an optional PPA BLEND implementation. Automatic
  GameRender dispatch requires `CONFIG_PXA_RENDER_ACCEL_INDEX8`, which defaults
  off: palette programming and cache synchronization can cost more than CPU
  lookup. Scaling, cursors, padded rows and shared destination cache lines keep
  their original CPU path.

PPA operations are blocking ownership boundaries. Keep input buffers immutable
and destination buffers exclusively owned until completion. A failed operation
requires repainting the whole requested region with the CPU. Successful calls
have completed DMA and the driver's cache synchronization before returning.
The component serializes its PPA operations and owns the background CLUT during
palette resolves; other indexed PPA users must coordinate their CLUT updates
through the same owner. Independent SRM users need no CLUT coordination.

Board code supplies resolution, stride, pixel byte order and panel transfer.
Pixel-art scaling uses SIMD nearest-neighbor expansion, preserving exact RGB565
colors. PPA SRM's interpolated scaling is not substituted for that operation.
Perspective texturing, depth tests and general triangle rasterization continue
to use the GameRender CPU renderer.

## Verification

`bash firmware/components/pxa_render_accel/tests/test_host.sh` checks unaligned
inputs, tails, in-place swapping and destination guards with sanitizers. The
GameRender raster tests also run with the platform fill hook enabled. Set
`PXA_TEST_ARTIFACT_DIR` to select an ignored local artifact directory.

For device differential tests, enable `CONFIG_PXA_RENDER_ACCEL_SELF_TEST` and
PXADB test control, then issue `PERF ACCELTEST`. This explicitly runs the pixel
tests on each core and verifies PPA fills and palette conversion against CPU
results. Blends are compared with LVGL across all opacity values, including
padded rows and masked fallback, and timed against LVGL's cached scalar loop
on both flat and varying backgrounds. It never runs during startup. Keep self-tests disabled in release
builds. Compare `PERF START display`/`PERF STOP` completion counts as well as
rendering time; application submission rate is not panel frame rate.

The same optional command measures the IDF CORDIC driver against native libm
and the LVGL integer trigonometry lookup table. It includes Q31 conversion and
driver locking, tests one, two and eight points per call, and checks numerical
error. Small batches bound the polling driver's interrupt-masked interval.
CORDIC is not a general libm replacement: its normalized inputs, convergence
range and approximate outputs differ. The CORDIC driver is referenced only by
this developer test; normal rendering does not acquire the engine.

PXA's C++ Game3D helper computes camera trigonometry and vertex projection in
the guest application, then submits screen-space primitives to GameRender.
Optimizing native rasterization does not replace guest sin/cos calls. Using
CORDIC there would require a guest-accessible native operation and application
integration, with boundary overhead included in the comparison.
