# PXADB2 Binary Transport

PXADB2 is the binary transport used by the desktop product simulator. PXADB1
remains the firmware protocol over USB Serial/JTAG, UART or board-initialized
TinyUSB CDC (ESP-Mosaico), so existing devices and
tools remain compatible.

## Framing

Every value is network byte order (big-endian). A frame has a fixed 10-byte
header followed by `payload_length` raw bytes:

```text
u32 payload_length
u8  frame_type
u8  flags             # reserved; must be zero
u32 sequence
bytes payload
```

The maximum payload is 1 MiB. Frame types are:

| Type | Name | Payload |
| --- | --- | --- |
| 1 | command | UTF-8 command line, without a trailing newline |
| 2 | response | UTF-8 `kind`, NUL byte, UTF-8 detail |
| 3 | upload-data | `u64 offset` followed by raw file bytes |

`upload-data` is limited to 64 KiB of file bytes. This avoids Base64 expansion
and allows a package upload to use much larger chunks than PXADB1 serial
frames.

## Session

Unix sockets rely on filesystem permissions. A TCP listener is disabled unless
explicitly requested and requires the first command to be:

```text
AUTH <token>
```

The service responds `OK` only after a constant-time token comparison. Then
the client sends `HELLO`; its response reports `protocol=2`, `binary=1`, and
the negotiated `max_chunk` value.

The current TCP mode authenticates but does not encrypt traffic. It is for a
trusted development LAN only; a future public-network transport must layer TLS
or another encrypted channel below these frames.

## Package Upload

The package workflow preserves the PXADB command contract:

```text
FSPUT <base64-path> <size> <sha256>
upload-data frames
PACKAGE deploy <app-id>
```

`FSPUT` returns `READY <remaining>`. Each binary data frame is acknowledged by
`READY` or `OK`; its offset supports retry and resume. `PACKAGE deploy` invokes
the same signed POSIX installation transaction used by the simulator's local
installer.

On serial USB CDC, the client sends separately sequenced PING keepalives while
waiting for PACKAGE deploy. This wakes pending IN responses after long flash/
filesystem operations; PING replies never count as the installation result.
Deploy still waits for its original OK/ERR sequence. If that result is lost,
the timeout reports that installation may already have completed and recommends
checking PACKAGES/logs. It does not diagnose an already-established session as
PXADB being disabled.

Before staging a local package, the client reads INFO storage metrics when the
firmware exposes them. Its LittleFS estimate includes the uploaded source, the
decoded installation files, file/directory metadata and commit headroom. An
upgrade must keep the current installed files until commit, so their space is
not counted as available. Insufficient space fails before any upload or inbox
mutation and reports the estimated deficit. Older peers without storage metrics
retain the existing behavior. This check is an estimate, not a reservation;
device signature verification and installation checks remain authoritative.

### Firmware receive lifetime and storage

Firmware `INFO` can additionally advertise `package_receive=1` and
`package_directory_adopt=1`. These are optional host capabilities; a simulator
or older firmware without them keeps its existing staging behavior.

On a capable host the CLI/GUI sends files into
`pxa-state/inbox/.receiving-<app-id>` (or `.receiving-<app-id>.pxa`). This reserved
candidate is invisible to the app catalog. Installation keeps one connection
and finishes with `PACKAGE deploy <app-id>`: the host authenticates the complete
candidate, checks its actual App ID, publishes it, and installs it in the same
command. An explicit `pxadb package stage` instead uses `PACKAGE publish
<app-id>` to retain a verified offline source for later installation.

Directory installation adopts the authenticated inbox tree into the existing
slot transaction by rename. It avoids a second copy of the decoded payload;
the transaction still verifies every incoming file before replacing current.
The admission estimate therefore reserves commit headroom rather than another
full directory payload. Container installation still needs the compressed
source alongside decoded incoming files: Container 0.1 authenticates the entire
encoded stream before decompression. This is not HTTP streaming decompression.

`FSABORT`, graceful `BYE`, a new `HELLO`, the firmware's 30-second inactive
control-session timeout, and cold startup discard unpublished receive sources.
The single in-flight file uses `/.pxadb-upload`, which is removed at those same
boundaries and after file checksum/write/commit errors. An abruptly disconnected
device can retain a partial file until timeout, reconnection, or reboot; it is
never visible as an installable app. CLI/GUI error and cancellation cleanup is
best-effort; host cleanup handles a lost connection.

Failed deployment removes its staging copy and recovers its slot transaction.
Cold startup recovers interrupted transactions before scanning apps, then
removes obsolete previous/session copies and legacy incomplete inbox uploads.
An unrecoverable transaction is retained and reported to preserve the last
recoverable application. Installed apps and private data are not temporary
files. Successfully published explicit staging sources survive disconnect and
reboot until consumed or explicitly removed.

## Simulator Debug Commands

Simulator application management uses `PACKAGE enable <app-id>`,
`PACKAGE disable <app-id>`, `PACKAGE clear-data <app-id>` and
`PACKAGE uninstall <app-id>`. A live full-system UI owns the operation and stops
the app before changing files. `OK accepted` acknowledges the request;
completion updates the app list and posts a system toast. Without a live control
socket, the offline installer helper returns `OK completed` after the change.
Errors from a live owner never trigger offline deletion. Uninstall removes the
user package and its private data; clear-data keeps the package. Disable state
persists across restarts and replacement installs. Built-in system apps cannot
be uninstalled. `PACKAGES` reflects the persistent enabled state.

The local simulator also supports `SCREENSHOT`, `INPUT CAPABILITIES`,
`INPUT POINTER <DOWN|MOVE|UP> X Y 0`, `INPUT TAP X Y`,
`INPUT SWIPE X1 Y1 X2 Y2 DURATION_MS STEPS`, `INPUT KEY <KEY>`, and
`INPUT SYNC`. Screenshot replies begin with PNG `META`, followed by offset
`DATA` response frames and `OK`. The client exposes these as `pxadb screenshot`
and `pxadb input`; raw PNG bytes remain inside the local control bridge and
are Base64 encoded only in the response records required by the existing PXADB
command contract.

## Firmware performance diagnostics

Firmware HELLO advertises `perf-raster,heap-local` when test control is enabled.
`PERF START` / `PERF STOP` record bounded raster and distinct FrameId LCD-complete
intervals. Check both overflow counters before deriving displayed FPS.
`PERF INPUT` drains the existing sampled-pointer latency counters, returning
`guest`, `present` and `visible` total microseconds, count, p95 and maximum in
three `DATA` frames followed by `OK`. Start an explicit `PERF START` window,
read `PERF INPUT` to clear startup/history, apply input, then read it at the end
and release the probe with `PERF CLEAR`. During this window the periodic display
consumer leaves the counters untouched; without it, a read may only cover the
remainder since the display's last drain. This reuses the optional raster probe
(2112 bytes on pai-touch), with no new permanent counters or persistent setting.
All stages start at the sampled input timestamp: `present` ends at submission
to the render pipeline; `visible` ends at completed LCD transfer. Captures must
also prove that the game surface is visible and that input changes pixels below
the FPS overlay. Counts can differ because delivery and presentation coalesce.
`tools/measure-device-game-input.py` performs that check; it does not measure
physical touch scanning delay before the timestamped synthetic sample.
`MEMORY START` resets ESP-IDF allocation minima for the current measurement;
`MEMORY STOP` returns the snapshot before restoring the boot-wide minima.
This includes allocations freed between snapshots, without a polling task or
sampling buffer. Do not treat boot-wide minima or Surface historical peaks as
the peak of the current app. Separate launch and warmed steady-state windows,
and always stop the local monitor when a measurement fails.
