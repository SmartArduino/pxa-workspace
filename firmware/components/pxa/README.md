# PXA Platform Component

[简体中文](README.zh-CN.md)

PXA means **Portable eXecutable Application**. It is the application's
portable execution and packaging model; **PXA Platform** refers to the Host,
service contracts, and tools that implement that model.

This component is the ESP-IDF integration for the consolidated PXA draft. The
normative protocol, portable `libpxa` core, application SDK and package tools
live in `pxa-system`; this directory owns only product-facing ESP host,
service, package and UI adapters. The
old JSON manifest, `PxaRuntime`, `PxaAppStore`, `PxaUiHost` and PXA-Wire
implementation are not part of this tree.

`CONFIG_PXA_ENABLED` enables the platform entry point. At boot,
`pxa_host_initialize()` initializes the managed Package root, recovers
interrupted slot transactions, and authenticates the signed Packages in
`CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_BUILTIN_PACKAGE_ROOT`. Factory Packages run
directly from that product-owned source; they are not copied into
`CONFIG_PXA_STATE_ROOT` at boot. Packages installed from Inbox sources use the
mutable managed directory
`CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_STATE_ROOT/packages/<publisher-root-hex>~<app-id>/`.
Installation temporarily uses
`CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_STATE_ROOT/packages/.session-<publisher-root-hex>~<app-id>/`,
then removes the session after commit or recovery. Downloaded Packages may be
staged in `CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_STATE_ROOT/inbox` and require an explicit install action. The canonical
Inbox form is one signed `<app-id>.pxa` container; directory Packages remain a
development compatibility input. Package listing reads the last authenticated
repository snapshot and does not rescan storage as a hidden side effect.
List clients query the snapshot count first and allocate only the bounded number
of records they will consume; count queries do not project DTOs or load icons.
The repository's larger internal metadata records grow in four-entry PSRAM-first
chunks up to the configured 64-App limit instead of reserving every slot at boot.
The disabled-App policy uses the same demand-grown approach: it starts empty,
keeps identities sorted for binary lookup, and publishes a RAM change only after
the complete newline-delimited NVS candidate has committed successfully.
Boot, explicit scan requests, PXADB package commits/removals, install and deploy
operations refresh the Inbox snapshot. The App Manager lists an authenticated staged
Package as pending installation, but it is never launchable until the installer
has verified the container before decompression and revalidated the decoded
file inventory while writing the atomic incoming slot. An Inbox source with
the same App ID as a product-owned factory Package is ignored; system Packages remain
the authoritative source for their IDs. Production installs remove a committed
Inbox container; development deployment may retain it for repeat tests. Private data uses
`CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_STATE_ROOT/data`; App enable policy is stored in NVS.
Each App's queued Work is persisted in that App's private data root under the
reserved Storage key `work.v1`, and is reloaded on its next launch in the same
monotonic boot epoch.

Package sources are always fully authenticated before direct factory use,
installation or update: the publisher signature and every manifest-inventoried
file digest are checked. By default, ESP treats a successfully committed
`packages/.../<publisher-root-hex>~<app-id>` directory as trusted Host-managed storage, so boot,
catalog loading and launch only parse its manifest and check its identity. Enable
`CONFIG_PXA_REVERIFY_INSTALLED_PACKAGES` when the platform cannot guarantee that
managed Package storage is only modified through the installer; this restores a
full signature and content-digest verification whenever an installed Package is
read. Interrupted session `incoming` directories are always fully verified for recovery.

The ESP32-S3 component includes a WAMR `ComponentEngine`. Launch resolves the
complete publisher/App identity, loads the managed `current` slot, selects
an exact AOT Artifact or portable Wasm fallback, then activates the main UI
Component through `ActivationCoordinator`. Guest callbacks run serially on a
dedicated pthread with bounded queues and a watchdog deadline. The deadline is
polled independently of the 5 ms frame clock; its default 50 ms period avoids
high-frequency idle watchdog callbacks while keeping timeout handling prompt.
`CONFIG_PXA_CALL_TIMEOUT_MS` defaults to 3000 ms; the existing timeout prompt
continues to handle callbacks that exceed that deadline.
Manifest IPC endpoint names are registered before the main Component starts,
but their provider Components are activated only when first called. If a
provider cannot be allocated, only that IPC request fails; the calling
Component and already-running providers continue running.
Window, UI 0.3,
Clock, pointer and system-back events use the same Draft service contracts as
the simulator. Owner-thread commands use type-specific bounded payloads, while
high-rate pointer moves use a separate compact mailbox that preserves edge
ordering and coalesces only contiguous movement. This keeps input bursts from
filling the reliable command queue and bounds the mailbox independently of
queue length. UI, pointer and Back work carries a process-monotonic instance ID
so delayed input is discarded after a Package switch. Net 0.2 provides
permission-gated HTTP(S) GET with HTTPS
certificate validation, no automatic redirects and a 4 KB response cap. Its
four pending PXA requests are serialized by one PSRAM worker, so DNS, TLS and
body reads do not stall the guest event, frame or touch-dispatch task. The
private Net module
owns request slots and transport synchronization; the runtime Host receives a
completion notification and polls results only on its owner thread. It records
slot pressure, queue failures, timeouts and response-memory peaks. Releasing a
slot resets only metadata under the cross-core lock, and streaming body copies
do not hold that interrupt-disabling lock while reading PSRAM.
The worker is created on the first request. Body and header storage is sized
for that request and released outside the lock after cancellation, failure or
Stream close, rather than reserved for all four idle slots.

The private KV backend loads its snapshot and allocates its two working buffers
on first use. Apps with neither declared Job Components nor existing KV snapshots
do not open KV to initialize an empty scheduler. Existing snapshots still undergo
scheduler validation so removed workers cannot retain stale queued work.
Setting a key to its existing value performs no file write;
changed values retain the synchronous durable A/B-slot commit behavior.

Canvas display lists reuse returned buffers after the backend releases its
previous frame. Stable frames reuse their image-reference set; new Canvas
assets are prepared outside the LVGL execution lock. `pxa_canvas_present_regions`
accepts up to four dirty rectangles in logical Canvas pixels, including the old
and new bounds of moving objects. The first frame redraws the whole Canvas;
later regions are scaled and clipped before LVGL invalidation. The original
present helpers retain full-Canvas redraw behavior. These optimizations add no
new application memory quotas or fixed image-cache budgets.

Audio 0.2 feeds bounded 16 kHz mono PCM frames through a dedicated internal-RAM
queue and worker to the product-provided media sink. They are created on the
first playback session, so Apps that never play audio do not reserve them.
Its platform module owns
provider sessions across Package switches, rejects stale queued frames, flushes
active voices during teardown, and records queue pressure and delivery metrics.

For a declared optional permission that has not yet been granted, the ESP host
posts a modal authorization request to the LVGL task and keeps the guest
`ACQUIRE` request pending without blocking the runtime thread. Allowing the
request persists the grant and returns its capability Handle; denying it
completes the request as denied. The App Manager can also change a running
app's policy: revocation invalidates issued permission Handles immediately.
For a running App this update is queued to the runtime owner thread, which
persists it once through the Permission service before changing active Handles;
the page callback does not perform a duplicate storage write.
Packages cannot add permissions or scopes at runtime; those remain part of the
signed manifest and require a signed package update.

Permission grants remain in checksummed per-App A/B files. The ESP adapter
keeps one bounded verified snapshot (at most 1664 bytes, allocated in PSRAM
when available), so loading a manifest with many permissions authenticates the
two policy slots once for that App rather than reopening them for every tuple.
Permission writes update the cache only after the new slot generation is
durable; a failed write cannot expose an uncommitted decision in RAM.

An active Package retains only its actual encoded manifest bytes. The POSIX
installer still enforces the 16 KiB protocol limit, but result buffers no longer
need to reserve that maximum; directory verification also allocates its
short-lived manifest copy at the authenticated file's bounded size. Container
verification uses the signed header size, and install comparisons hold only the
actual source and current manifest bytes. The repository starts without an
encoded-manifest buffer and grows a PSRAM-first high-water buffer only to the
largest bounded manifest encountered by this process.

The independent `pxa-system/simulator/desktop` target renders the standard
system UI without product firmware. It intentionally does not verify or
install `.pxa` containers and does not execute WAMR Guest code. This workspace
does not yet contain a product runtime simulator. When one is added, it must
share the device package verification, installation transaction and runtime
adapter contracts rather than mock package installation. See
[`simulator/README.md`](../../../simulator/README.md).

## Publisher trust

No publisher is trusted by default. Product firmware supplies a strong
definition of the C hook in `pxa/pxa_publisher_trust.h`. Each entry is the
canonical DER SubjectPublicKeyInfo of an ECDSA P-256 public key; the Package
publisher ID is SHA-256 of those exact bytes.

```c
#include "pxa/pxa_publisher_trust.h"

static const uint8_t publisher_spki[] = { /* canonical DER SPKI */ };
static const PxaTrustedPublisherKey publishers[] = {
    {publisher_spki, sizeof(publisher_spki)},
};

size_t pxa_platform_trusted_publishers(
    const PxaTrustedPublisherKey** output_keys) {
    *output_keys = publishers;
    return sizeof(publishers) / sizeof(publishers[0]);
}
```

The Zuowei development board enables
`CONFIG_PXA_TRUST_BUNDLED_DEVELOPMENT_KEY` so packages generated from
`pxa-system/apps/pxa` are accepted. That key is public development material and must not
be trusted by production firmware.

## Layout

- `../../../deps/pxa-system/libpxa/` is the portable C99 Core and its standalone host
  test suite.
- `../../../deps/pxa-system/spec/draft/` is normative for Core, Package, UI 0.3 and
  all other services.
- `../../../deps/pxa-system/sdk/guest-c/` is the C SDK targeting the two
  `pxa.core.v0` imports.
- `../../../deps/pxa-system/tools/` compiles Wasm/AOT Artifacts and creates signed
  Packages.
- `src/runtime/` owns the process/activation lifecycle, event loop and public
  host facade.
- `src/services/` owns ESP Audio, Net, Surface and activation service wiring.
- `src/package/` owns trust, repository, install policy, permissions and icons.
- `src/ui/` owns the neutral UI-shell bridge and product `app_pages` adapter.
- `include/pxa/` contains the stable public firmware facade. Together these
  directories provide the C ESP host, activation-scoped service
  wiring, private Audio/Net workers, the authenticated UI asset cache,
  neutral package/permission stores, Package Catalog and icon adapters,
  neutral UI-shell boundary, `app_pages` adapter, thin public facade and trust
  hook. Catalog metadata is copied under the repository lock, while signed PNG
  loading and its single-allocation LVGL resource lifecycle run outside it.
  Non-UI callers use the Host-owned package records and action enum without
  importing `app_pages`. The C
  installer, WAMR engine and LVGL UI adapters live in `libpxa/adapters/`.
- `tests/` contains focused host tests for ESP adapter state machines and
  backend boundaries. Portable SDK and App tests live under `pxa-system`.

Run the ESP adapter's host-only checks without invoking ESP-IDF:

```sh
firmware/components/pxa/tests/test_host.sh
```

App sources may live under `pxa-system/apps/pxa/<app-directory>/` or an
external root selected with `PXA_APP_SOURCE_ROOT`. Firmware builds never package
them. `tools/pxa/package_app.sh` explicitly creates `manifest.pxm`,
`signature.pxs`, portable Wasm, a target AOT Artifact, inventoried resources
and a single-file `.pxa` container for pxadb installation. Factory Packages
are created only by the explicit partition-image tool and run in place at boot.
See [PXA App Delivery](../../docs/pxa-app-delivery.md).

The manifest generator inventories every `artifacts/main.<target>.aot` present
in its staging directory, so one Package may contain several architecture AOT
Artifacts plus portable Wasm fallback. Artifact selection still requires an
exact target, engine and engine-ABI match before falling back to Wasm.

## Packaging and releases

`package_app.sh` writes both the legacy signed directory and the canonical
single-file `<output-dir>.pxa`. The `.pxa` uses independent 4 KiB LZ4 blocks,
so device installation needs bounded buffers and does not inflate the whole
archive in memory. `package.json` may set the signed positive integer
`release_sequence`; release automation must increase it for each published
update. The human-readable `version` is not used for ordering.

### Multi-file and WASI libc Apps

The legacy direct builder accepts either `"source": "main.c"` or a
`"sources": ["main.c", "logic.c"]` array per Component. For modules in
separate directories, use the CMake WASI builder:

```json
{
  "build": {"system": "cmake", "source_dir": "."},
  "components": [{
    "id": "main",
    "kind": "ui",
    "cmake_target": "pxa_main",
    "wasi": {"version": "preview1", "libc": "wasi-libc", "features": []}
  }]
}
```

The App `CMakeLists.txt` loads `PxaGuest` from `PXA_CMAKE_MODULE_DIR`, builds
folder-level code with `pxa_add_module()`, and combines those modules with
`pxa_add_component()`. A complete example is under
`pxa-system/apps/tests/cmake-wasi-app`. Build packaging requires an installed
WASI SDK and an explicit `WASI_SDK_DIR`:

```cmake
pxa_add_module(app_logic
    SOURCE_DIRS logic
    INCLUDE_DIRS logic/include)
pxa_add_component(pxa_main
    COMPONENT_ID main
    SOURCES main.c
    MODULES app_logic)
```

`SOURCE_DIRS` collects every `.c` directly in each named directory and uses
CMake `CONFIGURE_DEPENDS`, so adding or removing a source triggers a
reconfigure. It is intentionally non-recursive: give each subdirectory its own
module to keep module ownership and include paths explicit.

```sh
WASI_SDK_DIR=/opt/wasi-sdk \
  pxa-system/tools/package/package_app.sh <app-directory> simulator \
  simulator/assets/system/pxa/builtin/pxa-<app-directory>
```

PXA publishes WASI Preview 1 on ESP32-S3 and the simulator. A Component with
feature bits `0` can use `wasi-libc` routines that remain inside Wasm linear
memory, such as string, memory, formatting, parsing and allocation routines.
The Hosts additionally publish `monotonic-clock`, `wall-clock` and `random`.
Because Preview 1 selects a clock at call time through the shared
`clock_time_get` import, a Component using either clock must sign for both
clock features. Entropy and clocks are non-interactive device primitives: the
signed feature declaration is their authorization and they do not create a
runtime user prompt.

PXA does not publish ambient stdio, argv, environment or filesystem access.
Apps continue to use PXA services and their permission handles for files,
network, sensors, audio and other user/device resources.
The base context binds Guest descriptors `0`, `1` and `2` to a Host-owned null
device. This satisfies wasi-libc's retained formatting imports while reads
return EOF and writes are discarded instead of reaching the device console.
The signed Component requirement is checked before loading, and the engine
then checks every actual import against it; `env` imports, unknown modules and
undeclared WASI calls are rejected before instantiation. Feature names present
in the source schema but not listed as supported above are reserved for later
Host implementations and must not be advertised until their resource and
user-permission adapters are present. Wall-clock availability does not imply
that network time synchronization has completed.

On ESP32-S3, WAMR allocates runtime state and linear memory on demand from
PSRAM. PXA does not impose a separate global byte limit and does not fall back
to internal RAM. An allocation failure leaves existing allocations intact;
Guest allocation returns failure, while an instance that cannot be created
returns `PXA_STATUS_RESOURCE_LIMIT`. If a temporary event buffer cannot be
allocated, the event remains queued for retry and the running Component is not
stopped.

Each Component reuses an event scratch buffer that grows on demand. The adapter
resolves its native address again for every delivery because `memory.grow` may
relocate linear memory; the buffer is released with the Component instance.

`CONFIG_PXA_HOST_MANAGED_HEAP_SIZE` controls the per-Component heap that WAMR
inserts for Host-side temporary Guest-memory allocations. It is distinct from
the wasi-libc `malloc` heap and is not a limit on the Component's linear memory.
PXA-built Components export the linker heap boundaries so WAMR can place this
heap in unused space in the Component's last linear-memory page when possible.

Runnable CMake/WASI test Apps live under `pxa-system/apps/tests/wasi` so they do
not become factory Apps automatically. Package one by selecting that source
root explicitly:

```sh
test_root=/tmp/pxa-test-packages
PXA_APP_SOURCE_ROOT="$PWD/pxa-system/apps/tests/wasi" \
PXA_PACKAGE_OUTPUT_ROOT="$test_root" \
WASI_SDK_DIR=/opt/wasi-sdk \
  pxa-system/tools/package/package_app.sh wasi-libc-lab simulator \
  "$test_root/pxa-wasi-libc-lab"
```

`wasi-libc-lab` exercises libc and directory modules, `wasi-system-lab`
exercises the signed clock and entropy features,
`cmake-components-lab` exercises two independently instantiated Components,
and `wasi-undeclared-random` is a security negative that must package
successfully but fail activation with `PXA_STATUS_DENIED`.

For a planned publisher-key change, create the first lineage link with the old
and new P-256 private keys:

```sh
python3 pxa-system/tools/package/build_publisher_lineage.py \
  <app-id> <old-private.pem> <new-private.pem> <app-dir>/publisher.pxkl
```

Set `"publisher_lineage": "publisher.pxkl"` in that App's `package.json`, set
`PXA_SIGNING_KEY` to the new key, increment `release_sequence`, and rebuild.
For later rotations pass the existing chain through `--extend`. The old signer
is revoked by default; `--keep-old-signer` deliberately permits an interactive
rollback and should only be used when that policy is intended. Losing the old
private key cannot be repaired by silently replacing the publisher ID.

Host-side checks do not require ESP-IDF:

```sh
python3 pxa-system/spec/draft/tools/test.py
python3 pxa-system/tools/package/test_ui_vectors.py
bash pxa-system/tools/package/test_guest_sdk.sh
bash pxa-system/tools/package/test_package_tool.sh
```

## Product Simulator Trace

`PXA_SIMULATOR_TRACE` is reserved for the product runtime simulator described
above; there is no `app_pages_simulator` target in this workspace yet. A future
implementation may export its bounded decoded Guest-control and Host-event
trace as JSONL, without retaining raw payload data.

Factory provisioning writes `CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_BUILTIN_PACKAGE_ROOT`
without replacing `CONFIG_PXA_STATE_ROOT`. A complete storage-partition image
still clears App state when it is flashed, so it is a factory-only operation;
normal firmware updates and App development use pxadb instead.
