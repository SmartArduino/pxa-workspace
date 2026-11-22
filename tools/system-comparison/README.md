# Pai Touch: PXA / Aster comparison

These tools build actual guest applications for both systems. Measurements,
builds, serial logs and reports belong in an ignored
`local/artifacts/pai-touch-aster/<YYYYMMDD-HHMMSS>/` directory.

`common/bench_kernel.c` is shared without system-specific branches. Both builds
use WASI SDK 34, `-O3 -ffp-contract=off`, and the same Xtensa WAMR
compiler/recipe. Compare the two `bench_kernel.c.obj` SHA-256 values before
interpreting results. `common/bench_host.c` produces reference checksums only;
desktop execution speed is not a device score.
The surrounding applications use the recommended PXA C++ SDK and Aster C ABI.
Their complete binaries and background services are different even when the
shared Wasm object is identical. Linking can change AOT optimization: a module
containing `memory.grow` can inhibit memory-base/bounds hoisting in functions
with calls, including out-of-line `memcpy`. Identical Wasm objects do not imply
identical native instructions. Compute-only timings cannot establish
that a difference came from ABI dispatch: no system calls occur inside those
kernels.

The default profile retains `-fno-builtin`. To enable standard-library builtin
optimization, set `PXA_APP_DEFINES=PXA_BENCH_BUILTINS=1` for the PXA build and pass
`--enable-builtins` to `build_aster_bench.py`. Match this choice on both systems
for a framework comparison. A before/after SDK optimization experiment should
explicitly record differing compiler flags and must not be presented as an
identical-object comparison.

In a WASI application, `pxa_add_app(... BUILTINS ...)`,
`pxa_add_component(... BUILTINS ...)`, or `pxa_add_module(... BUILTINS ...)`
enables this optimization only for that target's sources. Benchmark both
compute and presentation workloads before adopting it: code layout and
scheduling can produce regressions even when memory loops become simpler.
Defaults and existing application binaries retain their previous behavior.

The five compute tests cover integer arithmetic, scalar floating point,
128-KiB sequential updates, dependent pointer chasing, and 64x64 matrix
multiplication. Each runs nine samples; discard the first two. Timings include
initialization/checksum work and scheduling/preemption. Aster's public clock
has 1-ms resolution. Frame tests use 296x240 RGB565: repeated constant fill,
changing-color fill, an integer pixel effect, and 128 rectangles drawn in the
guest. Each phase runs eight seconds, repeated three times. Discard its first
two seconds. Neither application exits or changes clocks during a timed phase.

PXA uses three mapped buffers and its public direct-scanout preference.
Aster uses its public ordinary RGB565 Surface and synchronous IDF present.
`PXA_BENCH_RELEASE_PACING=1` additionally draws on Surface-release events, to
measure throughput without waiting for the next 16-ms animation tick.
Keep that profile separate from the ordinary timer-paced results.

The PXA collector records independent `PERF START display` completion counters.
The Aster board counts complete, drained SPI transfers. These are LCD-transfer
completion rates, not optical scan/tear measurements. Guest submissions must
not be reported as completed/displayed FPS.

## Build and run PXA

From the workspace root, with `WASI_SDK_DIR` and `WAMRC` set to the matching tools:

```sh
tools/app.sh build pxa-bench --board pai-touch --target esp32s3 \
  --source-root tools/system-comparison --output "$artifact_dir/pxa-package" --aot-only
python tools/pxadb/pxadb.py package install "$artifact_dir/pxa-package/pxa-pxa-bench.pxa" \
  --port /dev/ttyACM0 --yes
python tools/system-comparison/run_pxa.py --port /dev/ttyACM0 --output "$artifact_dir/pxa-run"
```

Set `PXA_APP_DEFINES=PXA_BENCH_RELEASE_PACING=1` on the build command for the
release-paced profile. The collector unlocks the test device, returns Home to
dismiss Recents if the device was already unlocked, and keeps one
PXADB connection. The guest inhibits idle while foregrounded. No PXA firmware
rebuild or flash is needed.

Pass `--cpu-only` to `run_pxa.py` to stop after the five compute workloads.
Its metadata records the requested scope; a CPU-only run has no FPS result.
`--fill-only` stops after the first constant/changing-fill phases, retaining
their independent LCD completion windows and all CPU/checksum samples.
`compare_pxa_builds.py` compares repeated before/after build runs on the same
device, verifies the kernel/pixel checksums, and preserves per-run timings.
Use its optional `--display-reference` only for a separately labeled historical
reference; pass full baseline/candidate runs for a fresh paired comparison.

For run-to-run noise, `pxa_fill_noise.py` installs two already-built packages
on one explicitly identified device. Supply `--baseline`, `--candidate`,
`--checksums`, `--port`, `--expected-serial` and a fresh `--output` directory.
Before collecting data it records a fixed balanced random AB/BA sequence,
package/collector hashes, firmware identity, warmups and inclusion rules.
It uses actual LCD completion counters; bootstrap intervals resample paired
runs, and the randomization test respects the balanced order assignment.
Do not interpret frames or the two phases of a run as independent trials.
`--analyze-only --output <directory>` recomputes the completed analysis;
`plot_fill_noise.py <directory>` produces its static comparison chart.

`aster_fill_noise.py` accepts the same package/checksum/port/serial/output
arguments for the Aster device. Its `run_aster.py --fill-only` collector records
native counter snapshots after two warmup seconds and at each phase boundary.
The host command/response midpoint estimates each counter's time; round trips
and an independent guest device-clock completion rate remain in the data.
The USB serial and port selftest identify the board; this port has no shell
command for a runtime firmware hash. Neither runner reflashes or reboots.

Every fill run also executes all five CPU workloads with checksum validation.
`summarize_noise_suite.py --pxa <dir> --aster <dir> --output <json>` summarizes
their paired times and timing resolution alongside the display measurements.
CPU inference is exploratory, with Holm adjustment across five tests per
system; Aster's one-millisecond clock limits small-effect claims regardless of
how narrow a resampling interval appears. `plot_noise_suite.py <json>` plots
the two systems' repeated fill measurements together.

## Experimental compiler memory optimization

`deps/pxa-system/tools/wamr/build_memory_opt_wamrc.py --output <artifact-dir>`
builds an isolated compiler from the pinned WAMR and LLVM revisions. Its patch
separates runtime descriptors from guest payload bytes for alias analysis and
folds an address-overflow/bounds pair into one unsigned test when the immediate
offset is provably covered by the initial memory. Memory growth remains
supported; imported/shared/multiple memories retain the conservative path.
Installed compilers, the vendor checkout, firmware and engine ABI are unchanged.
Set `WAMRC` to the resulting `build/wamrc` when rebuilding an app.

The optional `--initial-bound-ranges` profile also describes the guaranteed
initial memory size on LLVM bound loads. This lets LLVM eliminate repeated
guards for static-array loops it can prove safe. Dynamic accesses still use
software bounds checks, growth remains supported, and imported/shared/memory64
modules retain the conservative path. This profile can help memory-heavy
workloads while slowing other code through changes in native code generation
and layout. It is off by default; measure the complete app before choosing it.

`test_wamr_memory_alias.py` takes `--wamr`, `--wamrc`, `--iwasm` and `--output`.
Add `--expect-bound-ranges` when testing the optional profile.
Its host runtime must export its native API (`-Wl,--export-dynamic`) and disable
hardware bounds checks (`WAMR_DISABLE_HW_BOUND_CHECK=1`) so the tests exercise
software traps and moving linear memory. The fixtures cover direct, indirect
and native growth, failed growth, payload aliasing, final-byte boundaries and
wrapping effective addresses, plus static-array loops and dynamic indices.
Run these before loading a new compiler's apps
onto hardware. Compare identical complete Wasm inputs to isolate its effect.

## Build and run Aster

Use an isolated checkout at `local/aster-firmware-pai-touch`, pinned to
`24adaff82c437c0530f55fdb7ba2b490a7348e8c`. `prepare_aster.py` verifies that
revision and applies the reviewed overlay; it does not alter PXA firmware.

```sh
python tools/system-comparison/prepare_aster.py
python tools/system-comparison/build_aster_bench.py --output "$artifact_dir/aster-package"
# Activate the same installed ESP-IDF used by the device build.
idf.py -C local/aster-firmware-pai-touch/boards/pai_touch/idf \
  -B "$artifact_dir/build" -DSDKCONFIG="$artifact_dir/build/sdkconfig" build
# Confirm the selected port's USB serial/MAC, then perform the authorized flash.
idf.py -C local/aster-firmware-pai-touch/boards/pai_touch/idf \
  -B "$artifact_dir/build" -p /dev/ttyACM1 flash
python tools/system-comparison/run_aster.py --port /dev/ttyACM1 \
  --package "$artifact_dir/aster-package/benchmark.ast" --output "$artifact_dir/aster-run"
```

The initial full flash replaces the partition table and data, including a FAT
image containing fonts and the benchmark package. Later `app-flash` preserves
that data; ASTERFS uploads use the real transaction installer. Never open a
second reader while flashing/verifying or while a collector owns the port.

## Port scope and interpretation

This is Aster's existing ESP-IDF backend adapted to ESP32-S3, not its Zephyr
S3 build. It retains Aster's actual LVGL/FLUX, WAMR, graphics, package manager,
filesystem permissions, native Wi-Fi and network services. The new board
implements JD9853 SPI output, landscape rotation, CST826 two-slot touch,
backlight and an internal FAT volume because Pai Touch has no SD slot.
Application fonts are a licensed subset, not the full 96-MB SD resource set.
JL701 audio, camera, ADC keys and battery reporting are not integrated.
Do not claim feature/idle-memory parity with the full PXA product firmware.

The SPI clock is 80 MHz, CPU 240 MHz and octal PSRAM 80 MHz. Aster uses a
blocking board presenter; PXA retains its existing pipelined/TE-aware board
presenter. Their performance difference is a result for these implementations,
not an inherent limit of either application ABI. Aster's original Xtensa AOT
cannot interrupt arbitrary compute-only loops; this port is experimental and
must not be treated as a hardened general-purpose installation target.

Vendor license notices remain in the pinned checkout. The MMU adapter uses
public IDF mapping/cache APIs, retains mapping ownership on failures, and
introduces no PXA runtime dependency into Aster.
