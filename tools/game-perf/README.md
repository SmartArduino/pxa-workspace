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
changes in `docs/design/pxa-game-execution.zh-CN.md` are explicitly unimplemented.
