# AOT alignment experiment

This is an opt-in compiler experiment against the WAMR and LLVM revisions pinned
by `deps/pxa-system/config/wamr.json`. It does not change the SDK's default compiler,
target features, bounds checks, application ABI or installed firmware.

The original compiler assigns alignment 1 to every nonconstant guest address.
On generic RV32 this expands even provably aligned word accesses into byte
accesses. The patch uses LLVM's known-bits analysis of the effective guest offset,
bounded by the same 8-byte linear-memory-base alignment already assumed by the
compiler's constant-address path. Unknown or unaligned offsets retain alignment 1.
Wasm alignment hints are not treated as guarantees.

Build an isolated compiler from the project root, using the already built pinned
LLVM dependencies:

```sh
git -C deps/pxa-system/wamr worktree add --detach "$PWD/local/wamr-known-alignment" 6a54186abc1c10c37c28e443107107c97af8f25f
git -C local/wamr-known-alignment apply --check "$PWD/tools/game-perf/wamr-known-alignment.patch"
git -C local/wamr-known-alignment apply "$PWD/tools/game-perf/wamr-known-alignment.patch"
cmake -S local/wamr-known-alignment/wamr-compiler -B local/wamrc-known-alignment -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_TARGET=X86_64 -DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
  -DLLVM_DIR="$PWD/deps/pxa-system/.pxa/llvm-344b928e67bfc1d07cb150609c55548e86a9bf19/build/lib/cmake/llvm"
cmake --build local/wamrc-known-alignment --parallel 4
```

`check_alignment.py --help` lists required local tool paths. Supply the candidate
compiler, pinned baseline compiler, WASI SDK, a host WAMR `libiwasm.a`, RV32
objdump and an output directory. It verifies shortened aligned RV32 reads/writes,
preserved byte access for unaligned addresses, and retained bounds branches. An
x86 AOT executable checks every byte alignment, the last valid word, out-of-bounds
loads/stores and address overflow. Reports include compiler and Wasm hashes.
These checks do not establish a device FPS improvement.

For a game comparison, pass the isolated compiler through `WAMRC` when invoking
`tools/app.sh`. Record the final AOT hash, original Wasm code-section hash, scene,
firmware and native measurements. Keep experiments out of SDK defaults until the
target-device comparison and broader compiler regression checks are complete.

## GameRender comparison profiles

Run from the workspace root with the App checkout at `local/pxa-apps`:

```sh
bash tools/app.sh build quake-frame-io --board doit-gamekit \
  --source-root tools/game-perf/profiles --target esp32s31 --output local/replay-frame-io
```

| Profile | World rendering | Output | Producer |
| --- | --- | --- | --- |
| `quake-projected` | per-material projected rows | Guest | original codec |
| `quake-frame` | multi-material packets | Guest | ordinary snapshot copies |
| `quake-frame-blit` | multi-material packets | native indexed resolve | ordinary snapshot copies |
| `quake-frame-bulk` | multi-material packets | native indexed resolve | bulk snapshots |
| `quake-frame-io` | multi-material packets | native indexed resolve | bulk file/cache reads and layout-proven command codec |
| `quake-probe` | runtime-selectable Guest or frame packets | independently selectable Guest or native resolve | sampled preparation timings in one private AOT |
| `duke-blit` | original Duke world renderer | native indexed resolve | native counters in existing input records |
| `ra-size` | original RA renderer | original output | source `-Oz` code-size diagnostic; not a device load pass |

These are opt-in measurements. A profile name does not establish a performance
improvement. Preserve the tested firmware, SDK/App revisions, AOT hash, actual
level readiness and complete frame window. The validation report in
`docs/validation/game-render-batches-20261020` records the comparisons.

Profiles compile the **current** App sources by default. For historical replay,
use the revisions recorded with that measurement. `PXA_GAME_APPS_ROOT` can point
to an isolated App worktree and the existing `PXSYS_ROOT` option can select an SDK
checkout, without switching the shared workspace. The original source embeds
build-time strings; code-section hashes and full artifact hashes are recorded
separately. Upstream updates should go through `common/recrof/tools/upstream.py`
and the original-function differential checks before another device measurement.

`quake-probe` is private instrumentation, compiled out of ordinary App builds.
Modes 0/1/2/3 select Guest-only, native resolve only, native world only, or both.
It reads one ASCII byte from `/data/pxa-render-probe-mode` after presentation;
for SD packages this can use the normal read-only App-media fallback. PXADB
cannot write the App-private directory. Write only the owned probe file under
the fixed `PXA/media/<AppKey>` root. FAT replacement may require removing that
owned file before upload; a missing or invalid file retains the previous mode.
Mode changes need a fresh actual-level record, then a separate warmup before
the next sample window. Timings are sampled on 1/16 engine frames and have
instrumentation overhead. They are not public-release FPS evidence.

## AOT memory lower bound

```sh
python3 tools/game-perf/aot_memory_budget.py \
  --reserved-linear 10027008 --idle-psram 15684068 \
  --output local/aot-budget.json path/to/first.aot path/to/second.aot
```

This read-only tool checks WAMR AOT v6 headers/section extents and compares TEXT
section bytes plus the supplied linear-memory reservation. It excludes other
allocations and does not validate imports, load an artifact, sign a package or
claim device performance. A positive remainder is not an admission pass.
The RA offline no-Asyncify comparison still has unresolved coroutine imports;
it must never be installed as though the new execution model already existed.
Measurements and limitations are in
`docs/validation/game-render-followup-20261020`; the proposed execution and audio
changes in that report were unimplemented at its measurement date.

## Core 2.1 main-loop candidates

`main-loop-probe` validates native waits, pointer/controller events, lifecycle,
terminal stop and queued replacement. `run_main_loop_probe.py` operates an
already installed probe; it neither flashes nor installs firmware. Record the
matching firmware image hash and run it before the game profiles. An optional
`--replace-with` names an existing app to check that replacement survives the
old call's cancellation.

The device must initially have no active packages. The probe requires input
ACKs containing `log_ms`, rejects older firmware before interactive input, and
uses that clock to exclude replayed history. `device_us` and framed log time
have different clock origins; a processed ACK is not Guest consumption.
See [input timing](../../docs/pxadb-input-timing.md) for the protocol semantics.

`quake-main-loop`, `quake2-main-loop`, `duke-main-loop`, `cnc-main-loop` and
`ra-main-loop` retain the original game settings and select
`PXA_RECROF_MAIN_LOOP=ON`. Set `PXA_GAME_APPS_ROOT` to the matching Apps checkout
and `PXSYS_ROOT` to the Core 2.1 SDK. These signed packages require one `main`
UI component with `execution_model: main-loop` and a Host advertising that
engine execution model. The normal Apps build still defaults to callbacks.

For example, from a checkout containing these profiles:

```sh
PXA_GAME_APPS_ROOT=/absolute/path/to/matching/pxa-apps \
bash tools/app.sh build quake2-main-loop --board doit-gamekit \
  --source-root tools/game-perf/profiles --target esp32s31 --aot-only \
  --output local/main-loop-q2
```

Unlike the old offline import-stripping size experiment, these candidates use
the real event-port SDK and omit the Asyncify pass and continuation buffer.
The RISC-V package build verifies their imports and memory bounds. Successful
compilation, desktop file/input tests, reduced AOT size and a positive static
memory remainder do not qualify device loading, FPS or original audio fidelity.
Those initial main-loop profiles used the legacy 16 kHz mono audio path.

## Synchronous Host audio candidate

`quake-host-audio` and `quake-host-audio-baseline` keep the native main loop,
original-rate PCM, exact lighting cache and indexed Host output identical.
Only the former enables `PXA_QUAKE_HOST_AUDIO_MIX`: Audio 0.11 accumulates
sample spans into the engine's existing S32 stereo buffer synchronously.
Both profiles use package version 0.4.15/sequence 135 and require Audio 0.11;
keep artifacts in separate output directories and identify them by SHA-256.
The ordinary Quake build keeps this option off pending device qualification.

The Host kernel retains no Guest pointers and adds no persistent PCM copy.
The engine still owns scheduling, loops, final volume and output quantization.
This is not an asynchronous Host voice mixer or an OPL synthesizer. Run
`local/pxa-apps/common/recrof/tests/run_quake_host_audio.py` after upstream
updates: it extracts the current vendored mixing loop rather than relying on
a separately copied reference. Correctness tests and successful AOT builds
do not establish a frame-rate gain.

## Exact source PCM candidates

`quake-native-pcm`, `quake2-native-pcm`, `duke-native-pcm`, `cnc-native-pcm`
and `ra-native-pcm` enable both `PXA_RECROF_MAIN_LOOP` and
`PXA_RECROF_NATIVE_PCM`. They require Core 2.1 and Audio 0.10. The signed
package IDs remain the corresponding `*-sd` app IDs, so the fixed SD resource
root and private saves remain associated with the same publisher and app.
The profiles retain the original renderer settings.

Use SDK `b787d4a` and Apps `5281572` or descendants containing those changes.
Quake, Quake II and Duke submit their original 11025 Hz stereo PCM; C&C and
RA submit 22050 Hz stereo PCM. The GameKit output worker pulls the Host mixer
at its 48000 Hz mono device clock. Bounded resampling and downmixing happen
at that boundary, with mixer storage charged to PSRAM.

Build with the same command as above, replacing the profile name. All five
signed artifacts, source identities, Node actual-level/PCM retry checks and
the real-device main-loop lifecycle probe are recorded in
`docs/validation/game-native-pcm-20261020`. Node timing is not device FPS.
Exact PCM negotiation alone does not prove uninterrupted physical playback
or the >=15 FPS acceptance target.

The device probe checks the visible canvas as well as fresh, distinct
heartbeats after each activation. A retained main-loop callback must not
leave the launch overlay covering the app. PXADB `INFO` now includes the full
running ELF SHA-256 so measurements can be bound to the actual firmware.

`quake-native-lit-cache` adds the existing exact-pixel lighting-row cache to
the native-loop/PCM profile. It is an explicit experiment. The 200,000-case
reference-kernel comparison includes all eight mip/texture variants, negative
and fractional stepping, fullbright and output guards. Record the compiler
used as a separate factor: the current candidate uses the isolated alignment
compiler, whereas the normal SDK compiler remains unchanged. Desktop pixel
equivalence and successful signing do not establish a device FPS gain.

## Native indexed output with the foreground main loop

`duke-native-blit` combines the native-loop/PCM profile with
`PXA_RECROF_HOST_BLIT`; `quake-native-lit-blit` adds the same output path to
`quake-native-lit-cache`. Both declare GameRender 1.2's
`borrowed-indexed-blit` feature. These profiles retain the original world
renderer, resolution, palette, audio format and fixed SD resource roots.

Each frame passes a 64-byte description of the existing indexed source,
512-byte palette and acquired RGB565 surface to the Host. The synchronous
operation borrows guest memory for this call, validates bounds and aliasing,
then resolves directly into the existing destination. It does not allocate
another framebuffer or copy the source image into a command packet. The
320-pixel output uses paired RGB565 stores; scaling and cursor cases retain
their exact sampling rules. A rejected frame uses the existing guest resolve.

Use the same firmware and AOT compiler for both sides of a comparison. Check
fresh actual-level records, a complete 60-second FPS window, and active Host
batch counters with zero rejected batches. Duke logs cumulative counters;
Quake logs period deltas and divides execution time by engine frames.
Compare `draw_us / frames`
as well as FPS: this timing includes acquisition and presentation, not only
palette conversion. Host execution counters isolate the native portion.
Run `local/pxa-apps/common/recrof/tests/run_host_indexed_blit.py` for the
ASan/UBSan pixel, scaling, cursor, odd partition, aliasing and guard checks.

Device results and the exact signed artifact identities are recorded in
`docs/validation/2026-10-20-game-fastpath.zh-CN.md`. Keep the earlier
`duke-blit` callback profile separate when interpreting those results.

## Indexed raster kernel device comparison

`render-kernel-probe` is a private Core 2.1 application, using existing
GameRender 1.2 borrowed indexed spans. Build/install its signed package once,
then run the same artifact against each explicitly identified firmware:

```sh
bash tools/app.sh build render-kernel-probe --board doit-gamekit \
  --source-root tools/game-perf/profiles --target esp32s31 --output local/render-probe
python3 tools/game-perf/run_render_kernel_probe.py --port /dev/serial/by-id/DEVICE \
  --serial MCU_MAC --elf FULL_ELF_SHA256 --output local/render-baseline
```

The runner requires an idle device and the ACK log clock, and rejects missing,
conflicting, failed or incomplete execution records. It runs eight cases three
times: horizontal lit spans, diagonal spans, fullbright spans, and short spans,
for power-of-two and arbitrary texture dimensions. Each case has ten warmups
and 100 measured batches. Telemetry separates native validation/execution from
wall time, which also includes event checkpoints. Compare output hashes across
firmwares as well as within repeats. This is a compute benchmark, not game FPS.
The unchanged caller-owned output/source buffers stay borrowed until both cores
complete the call. No probe control operation is added to the public ABI.

## Quake Host preparation and light-grid cache

`quake-host-prepare`, `quake-light-cache`, and `quake-light-cache-fast` are
private comparison profiles for one signed `pxa-quake-sd` application. The last
profile also enables large-read `memory.copy` and the layout-checked command
codec. Use the same built AOT when changing probe mode or Host firmware.

```sh
PXA_GAME_APPS_ROOT="$PWD/local/pxa-apps" bash tools/app.sh build quake-light-cache-fast \
  --board doit-gamekit --source-root tools/game-perf/profiles --target esp32s31 \
  --aot-only --output build/quake-probe
python3 tools/game-perf/run_quake_host_probe.py quake build/quake-run \
  --elf FULL_ELF_SHA256 --version INSTALLED_VERSION --mode 7 --controls
```

This device-specific runner guards USB `5CFF043509`, MCU `1c:29:04:d0:af:5e`,
firmware identity, installed version, and idle state. It writes only the owned
probe-mode file in Quake's fixed App-media directory and removes it on exit.
Mode is one hexadecimal byte, with these independent bits:

| Bit | Meaning |
| --- | --- |
| 1 | Native indexed output |
| 2 | Native world frame batches |
| 4 | Packed light-grid cache |
| 8 | Rebuild cache hits with the original engine and compare sampled cells |

Common modes: 1 = Guest world/native output, 3 = native world/output,
5 = Guest world/cache, 7 = native world/cache, 13/15 = corresponding cache
verification. Verification adds work and is not a performance configuration.
Mode files affect private probes only; ordinary builds do not read them.

Runs capture a 60-second LCD completion window at START `544,288,50`, HP=100,
require at least eight fresh level records spanning 45 seconds, and retain
original device timestamps when recovering dropped logs. Screenshots sit
outside the timed window. `--controls` also checks short press/release edges,
actual forward/backward movement and touch/controller overlay transitions.
Use cold starts with the same application catalog for firmware comparisons;
record both idle and in-game memory. Do not add nested stage timings or infer
FPS from an isolated raster kernel. Light-grid verification excludes only the
upstream packer's unsampled last row; all sampled packed words remain strict.

For a normal Quake package, omit `--mode` and use `--expect-light-cache` when
the package enables that cache. `--idle-lock` additionally waits 130 seconds
without input, captures the lock screen, swipes to unlock, and requires a
new level record from the same firmware. This option assumes this board's
persisted 120-second lock timeout; inspect both lock/unlock screenshots before
claiming lock coverage. PCM negotiation is checked, but this is not a physical
audio waveform or listening test.

The qualified firmware, source hashes, cold-start helper, comparison windows,
and production package results are recorded in
`docs/validation/2026-10-21-quake-host-prepare.zh-CN.md`.
