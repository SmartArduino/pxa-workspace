# PXA Layering and Portability Plan

Status: draft for review. Goal: make the entire PXA platform a standalone
pure-C99 package that can be embedded on any platform (ESP32, Linux desktop,
other RTOS, bare metal), with ESP-IDF integration reduced to a thin layer.

## 1. Current state

The platform is split in two:

- `libpxa/` — already a platform-neutral C99 core (runtime, wire, package,
  activation, slot transactions, all service wire modules) with callback
  backend tables and a standalone CMake build. No ESP-IDF/RTOS/FS/crypto/UI/
  engine dependency.
- `components/pxa/` — the ESP-IDF integration. Its active component build uses
  the C Core, C WAMR engine adapter, C LVGL UI adapter and C ESP host.
  The public `pxa_host_*` facade and development trust provider are C. The
  desktop simulator shares the same ABI and is the second reference host.

## 2. Target layering

```text
Layer 0  Spec (normative, no code)
         pxa-system/spec/draft/      Consolidated current protocol draft.
         Language-agnostic; frozen under the draft-to-1.0 rule.

Layer 1  Core library (pure C99, portable)
         pxa-system/libpxa/  -- published as the "pxa" package.
         No I/O, no OS, no allocator use after init, single owner thread.
         All platform access goes through callback tables:
           - service backends: fs, storage, net, audio, window, sensor
           - component engine: instantiate/start/stop/destroy
           - provider callbacks: signature verification, clock, entropy
         Provides: wire codec, request/handle/event model, package parsing
         and validation, deterministic artifact selection, activation
         planning, recoverable slot transactions.

Layer 2  Port adapters (C, per platform, optional)
         Implementations of the Layer 1 callback tables:
           - POSIX adapters (reference): FS installer, private files,
             storage, signature (OpenSSL or feature-less stub)
           - WAMR engine adapter: pxa_component_engine_t over the WAMR C API
           - LVGL UI adapter: direct tree/Canvas backends over the LVGL C API
           - ESP32 adapters: LittleFS backends, mbedTLS signature
         A minimal headless host (no UI, no network, no audio) must be able
         to run lifecycle + scheduler + private-data services unchanged.

Layer 3  Host integration (per platform)
         Boot sequence, threading model, clock/watchdog, dispatch loop:
           - mount FS, recover interrupted slot transactions
           - sync signed built-in packages, verify, install
           - start engine, host service loop (poll backends, drain events,
             enforce leases and deadlines)
         The ESP32 C host is the reference Layer 3 implementation.
         The desktop simulator is the second reference implementation and
         keeps exercising the exact same ABI and package model.

Layer 4  Apps
         Signed Packages (Wasm / AOT artifacts). Unchanged by this plan.
```

Direction of dependency is strictly downward. A layer may only depend on the
layer immediately below it plus Layer 0 (protocol constants).

### ESP Host lifetime boundaries

The reference ESP Host has two explicit ownership scopes:

- Process-owned resources are created before the owner thread starts and stay
  alive across Package switches: the command queue and pointer mailbox, owner
  task, timers, monotonic instance sequence, WAMR runtime/allocator, retained
  LVGL adapter, Audio worker and Package repository.
- Activation-owned resources are created after an authenticated manifest has
  been selected and are destroyed on every launch failure, replacement and
  normal stop: manifest storage, Runtime contents, service/adaptor workspaces,
  activation plan/coordinator and Package UI assets.

Activation workspaces are slices of bounded, aligned arena blocks. Manifest
storage remains in two adopted blocks because its views must survive for the
whole activation. The owner releases all physical blocks in reverse order only
after service adapters have been deinitialized. No activation failure path may
free an individual workspace or bypass the common stop path. Logs distinguish
logical workspace count, physical block count, used bytes and reserved bytes so
arena slack and fragmentation remain observable.

`src/services/pxa_esp_audio.c` is the sole owner of the Audio worker, queue, synchronization,
physical sink and provider-session slots. Provider-session IDs remain
process-monotonic so queued frames from a stopped activation cannot become
valid for a later Package. The Host obtains a backend table from the module and
resets only its activation sessions. Queue high-water, full-queue drops, stale
frames and renderer rejections are exposed in a bounded snapshot.

`src/services/pxa_esp_net.c` similarly owns the HTTP(S) worker, queue, request slots, transport
callbacks and synchronization. The owner Host receives only a completion
notification and polls the portable Net service on its own thread. Stopping an
activation cancels queued/running work and releases completed/streaming slots;
the process worker is retained for the next Package. Slot storage, concurrency,
queue failures, response bytes and timeouts remain observable before changing
the fixed bounded buffers to a different allocation policy. Slot payload
buffers are retained and only their metadata is reset under the cross-core
critical section; PSRAM body copies execute outside that section.

`src/ui/pxa_esp_ui_assets.c` owns authenticated Package-file lookup, PNG validation,
LVGL image descriptors and the bounded LRU cache. Activation state supplies
only the authenticated manifest and Package root. Cache bytes, references,
hits, misses, evictions and the largest RGBA decoded-image upper bound are
snapshotted before teardown; resources are dropped from LVGL before their
backing bytes are released.

`src/services/pxa_esp_services.c` owns construction, registration and teardown of the
activation-scoped service bundle. It wires permission, private FS/KV, IPC,
lease, sensor, network, audio, scheduler, window and UI services from one
configuration object; the Host keeps runtime coordination and event-loop
behavior. Fixed service workspaces come from the activation arena, while
service-specific deinitializers release dynamic or OS-owned resources before
that arena is reset.

`src/runtime/pxa_host_clock_slots.c` is a platform-neutral state machine used by the ESP
timer bridge. It advances deadlines without drift, allows at most one queued
Tick per Component slot, and uses a generation value so queued work from a
stopped or reconfigured Component cannot target its successor. The ESP Host
holds its clock lock only while changing this metadata; FreeRTOS queue calls
and Core event delivery remain outside the lock.

`src/runtime/pxa_host_command.h` gives every owner-thread command a type-specific union
payload instead of carrying fields for every command kind. The command is
compile-time bounded to 80 bytes. High-rate pointer input does not consume the
reliable FreeRTOS queue: `src/runtime/pxa_host_pointer_mailbox.c` stores only compact
pointer events in a 336-byte bounded state machine, coalesces contiguous moves,
and never coalesces down/up edge events. Its algorithm is platform-neutral and
tested independently; the ESP wrapper holds the cross-core lock only while
updating or copying mailbox metadata. Pointer, UI and Back commands carry the
process-monotonic main-instance ID, so delayed work cannot target a successor
after a Package switch, including when the successor has the same identity.

`src/ui/pxa_esp_ui_shell.h` is the runtime Host's product-UI boundary. The Host posts
neutral permission, timeout and toast models and obtains opaque font pointers
through this interface; it does not include `app_pages.h` or LVGL transitively.
`src/ui/pxa_esp_app_pages.c` owns callback registration and action conversion.
`src/package/pxa_esp_package_catalog.c` projects neutral repository records and permissions
into page DTOs, while `src/package/pxa_esp_package_icon.c` owns catalog PNG allocation,
validation, LVGL descriptors and release callbacks. The Package Store does not
contain page, Host DTO, LVGL or UI-shell dependencies. Built-in synchronization
publishes completion through a neutral callback that the App Pages adapter binds
to its refresh request. The Store copies a consistent metadata record
under its lock; Catalog and Host each own their output projection. The Catalog
then resolves a cached icon source and performs file I/O after the lock is
released. Catalog refresh therefore neither reparses every signed manifest for
its icon path nor blocks package transactions while reading PNGs; each icon
uses one combined descriptor-and-bytes allocation. The public Host API owns its
app-management enum, so MCP and PXADB do not include `app_pages.h` merely to
submit repository commands. Package, Host and page list APIs expose cheap
count queries, so App pages, permission details and PXADB allocate for the
current bounded result instead of their maximum capacity. Permission summary
refresh reads only the selected App's declarations and does not load catalog
icons. The repository metadata table grows in four-entry PSRAM-first chunks;
the configured 64-App limit remains a hard bound without imposing roughly
69 KiB of empty entry storage at boot. Repository snapshot visits are read-only
and never rescan the Inbox. Explicit Host refresh operations own directory traversal and
source authentication; PXADB invokes the Inbox-only refresh after atomically
committing a `.pxa` container or development `manifest.pxm`, and after removing
Inbox content. In-progress `.pxadb-part` files are ignored by the scanner. A
running App's
permission change is queued once and persisted by the owner-thread Permission
service; inactive App policy updates may write through the repository directly.

`src/package/pxa_package_disabled_policy.c` owns the platform-neutral sorted disabled-App
set, bounded growth and durable-before-RAM update rule. Its host tests cover
normalization, capacity and failed persistence. `src/package/pxa_esp_package_policy.c`
adapts that state to PSRAM-first allocation and the existing NVS blob format.
Consequently the Package Store no longer embeds a 64-entry (~4 KiB) disabled
table or implements NVS serialization itself; an empty policy allocates no
identity table.

`src/package/pxa_esp_permission_store.c` keeps one process-owned, mutex-protected verified
policy snapshot in PSRAM when available. Its compile-time bound is 1664 bytes.
The first access for an App reads and authenticates both A/B policy slots;
subsequent tuple loads for that identity use binary search over the cached
digests instead of allocating a parse workspace and reopening both files.
Writes mutate the spare cached snapshot and publish it in RAM only after the
new generation is durable, preserving disk/RAM consistency on failure. Cache
replacement reauthenticates the next identity. A missing legacy NVS namespace
is remembered for the process lifetime, while devices that still contain the
legacy namespace keep per-tuple lazy migration semantics.

Built-in package synchronization retains a separate short-lived parse workspace
so it cannot race foreground permission parsing or hold the metadata lock across
signature and inventory I/O. Each candidate, POSIX directory verification and
the active Host allocate encoded manifest bytes at the actual bounded file size;
the 16 KiB value remains a protocol rejection limit rather than a required
result-buffer capacity. Container verification grows its buffer to the signed
header's manifest size, and source/current comparisons use two exact bounded
reads without widening the Store lock. The repository starts with no encoded
manifest allocation and grows a PSRAM-first high-water buffer only when a
bounded source actually requires it.

Activation internals stay on the runtime owner thread. UI, app-management and
public API callbacks read a small process-owned published snapshot containing
only Package identity, process-monotonic main-instance ID and permission count.
Starting, stopping or hiding the main Component replaces this snapshot
atomically rather than exposing mutable service or coordinator pointers across
threads.

## 3. Boundary contract

Rules that keep Layer 1 portable and testable on the host:

- `libpxa` never opens files, creates threads, reads clocks, verifies
  signatures, does network I/O, renders UI or loads Wasm. Everything goes
  through registered callbacks that execute synchronously on the Core owner
  thread; asynchronous provider completion must be posted back to that thread.
- Single-owner-thread execution: interrupt handlers and worker threads enqueue
  work to the host runtime instead of calling libpxa directly. Core operations
  stay lock-free.
- The base Runtime does not allocate after `pxa_runtime_init()`: the caller
  supplies its workspace and `pxa_runtime_limits_t`. Dynamically sized
  services such as UI use separate explicit allocator callbacks and byte
  budgets, so capacity is a Host policy rather than an ABI count limit.
- Borrowed views: encoded manifests and activation-plan workspaces must outlive
  every activation referencing them.
- Public headers only include `<stdint.h>`/`<stddef.h>`; exported symbols use
  the `pxa_` prefix (ABI.md). Forward-extensible structs carry `struct_size`.
- Engine cleanup rollback contract: `destroy` after every `instantiate`
  attempt, `stop` with `PXA_STOP_FAULT` before `destroy` after a failed start.

## 4. Gap table (what must still be ported)

| Piece | Today | Target |
| --- | --- | --- |
| runtime / wire / package / activation / slot_transaction | C `libpxa` | Maintain portable Core coverage in `libpxa` tests. |
| POSIX installer, private-files backends | C reference adapters in Layer 2 | C adapters, with ESP LittleFS VFS shim. |
| ESP LittleFS backends | C adapters in Layer 2 | Device persistence validation. |
| Signature verification | C provider callback + ESP mbedTLS/OpenSSL adapters | Device verification with product trust keys. |
| WAMR engine host | C `pxa_component_engine_t` adapter plus C ESP host | Device lifecycle validation. |
| LVGL UI / window bridge | Direct UI 0.3 backend with Canvas and VirtualList plus Host environment source | Device rotation and lifecycle validation. |
| Service hosts (audio, net, sensor, permission, window, ipc, storage, fs, lease) | C registration; HTTPS Net and Audio media-sink backends are live; sensor is intentionally unavailable | Bind actual sensor hardware and validate Net/Audio on device. |
| Work queue | C POSIX Storage-backed adapter using per-App `work.v1` | Device restart policy validation. |
| Boot / host glue (`pxa_host_*`) | C host with a public C facade | Device lifecycle validation. |

## 5. Repository layout

Treat `pxa-system` as the repository boundary and keep `libpxa` as the portable
library name. The product repository consumes it through the ESP adapter below.

```text
pxa-system/libpxa/
  include/pxa/...          public C API (Layer 1)
  src/                     core implementation (C99)
  adapters/                optional Layer 2 reference adapters:
    posix/                 fs, storage, scheduler persistence, installer
    wamr/                  pxa_component_engine_t implementation
    lvgl/                  window/UI 0.3 backends
  tests/ bench/ cmake/     host tests, benchmarks, package config
components/pxa/
  include/pxa/             stable product-facing C facade
  src/runtime/             Layer 3 lifecycle and owner-thread host
  src/services/            Layer 2 ESP service providers
  src/package/             trust, persistence and package projection
  src/ui/                  product UI bridge
pxa-system/spec/draft/     consolidated normative draft
pxa-system/sdk/            guest SDKs
pxa-system/tools/          packaging and WAMR overlay tooling
pxa-system/simulator/      headless and SDL2/LVGL reference hosts
```

Portable adapters stay in `libpxa/adapters`; ESP/product adapters stay in
`components/pxa`. This keeps the future standalone repository free of product
headers and ESP-IDF dependencies.

## 6. Migration milestones

- M0 (done): libpxa is the Core implementation with standalone C host tests.
- M1 (done): the POSIX Layer 2 adapters are C: installer, private-files FS
  backend, storage backend and durable scheduler store
  (`libpxa/adapters/posix/`) plus the OpenSSL signature/hash adapter
  (`libpxa/adapters/openssl/`). Gates: `pxa_adapters_test` passes under `ctest`
  with ASan/UBSan.
- M2 (adapters done; device gate remains): the C WAMR engine adapter exists
  (`libpxa/adapters/wamr/`, `pxa::adapters_wamr` over any WAMR source tree),
  and the direct C LVGL UI 0.3 adapter exists (`libpxa/adapters/lvgl/`,
  `pxa::adapters_lvgl` over any LVGL 9 source tree). Its integration test
  exercises tree creation, partial patch, subtree replacement, Canvas,
  VirtualList events and teardown against real LVGL. All pass under
  ASan/UBSan. Remaining gate: ESP32 device validation.
- M3 (C service wiring closed on the host; device backends remain): the
  per-service validation path is closed in libpxa for storage, window, UI,
  IPC, permission, sensor, lease, scheduler, net, audio and Core. The remaining
  gate is that per-service validation apps (`pxa-*-lab`) pass on ESP32.
- M4 (simulator host ported to the C stack): the desktop
  simulator no longer uses the C++ core. `simulator/pxa_c_host.c` implements
  the `simulator_pxa_*` interface entirely on libpxa: the C installer installs
  the signed built-in apps, all C services run with simulated backends, the C
  WAMR engine runs the apps, and the C LVGL UI backend renders them. Both
  simulator self-tests pass (`--pxa-management-self-test`,
  `--pxa-trace-self-test`). The ESP host is also on the C services and
  adapters; the remaining device gate is that per-service validation apps
  (`pxa-*-lab`) pass on ESP32.
- M5 (in progress): the ESP component, public host facade and bundled
  development trust provider are C, and legacy C++ Core copies are removed.
  Close publishability after the ESP
  device gates with a porting guide and host/simulator/ESP CI matrix.

## 7. Porting checklist (new platform)

1. Memory: supply workspace; provide platform allocator only if loading
   package bytes / AOT artifacts is required (engine concern).
2. Clock: lease expiry, scheduler deadlines, watchdog; monotonic time source.
3. FS backend: read-only package storage + app private data (quota aware).
4. Signature provider: verify `signature.pxs` over the manifest; the
   publisher-trust hook is a separate small C function.
5. Engine adapter: instantiate/start/stop/destroy over any Wasm engine.
6. Optional services: net, audio, sensor, UI — register only what exists;
   headless operation must not require them.
7. Host loop: dispatch backends, drain events into engine memory, enforce
   per-component deadlines, own the single owner thread.

## 8. Open decisions

- Full C end-to-end: WAMR, LVGL and LittleFS all have C APIs. The active ESP
  component is C-only.
- Where the reference adapters live (in-repo `libpxa/adapters/` vs separate
  repos) — revisit at first external consumer.
- Whether layer 2 providers (e.g. audio graph, net fetch) are part of the
  published package or delivered as examples. The microbench and ABI policy
  already assume libpxa does not own platform callbacks.
- Keep the simulator as the permanent second reference host: it satisfies the
  draft-to-1.0 rule (two host implementations) and exercises the exact same
  Layer 1 code path as the device.
