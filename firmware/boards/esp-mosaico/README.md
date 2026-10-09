# ESP-Mosaico

This board profile implements `pxa_board_port_t` for the ESP32-S31-based
ESP-Mosaico. It uses the local ESP-IDF main checkout with CMake version 6.2.

Mosaico initializes its display before mounting product storage and shows a
small boot screen before codec, Wi-Fi, catalog and font setup. System UI
creation removes that screen. Wake requests are handled by the UI timer after
input/lock callbacks complete. A touch or button wakes the visible lock screen;
the waking touch sequence, including its release and a second contact, is
consumed. A new upward swipe unlocks it. Unlock restarts the idle interval and
restores the saved brightness without changing the saved level.

LVGL lifecycle, display buffers, rounding and synchronization use
`espressif/esp_lvgl_adapter ^0.7.2` (the lock resolves 0.7.2). Other boards
continue to use their existing adapter. The shared integration acquires the
selected board's display lock before creating the system UI.

## Hardware

The first 16 bits of `ESP_EFUSE_USER_DATA` select the hardware revision.
Unknown or unprogrammed revisions stop initialization before revision-dependent
GPIOs are driven. No eFuse is written by this firmware.

| Revision | eFuse value | LCD reset / clock | I2C SDA / SCL | Codec power |
| --- | --- | --- | --- | --- |
| V1.0 | `0x0100` | 42 / 44 | 0 / 1 | 56, active high |
| V1.1 | `0x0101` | 44 / 42 | 56 / 3 | Not used |
| V1.2 | `0x0102` | 44 / 42 | 56 / 3 | Not used |

- CO5300: 480 × 480 RGB565, QSPI at 48 MHz, CS 50 and data 36 / 51 / 35 / 9.
  Two PSRAM draw buffers use partial refresh with 4-column / 2-row-aligned areas.
  A dedicated flush task feeds two 16-row internal DMA buffers, never the
  PSRAM draw buffer directly. LVGL can render into its other draw buffer while
  SPI is busy; band copies overlap DMA. Completed scanout is converted to
  screenshot RGB565 only on request, avoiding an extra full-frame write.
  The two staging buffers together retain the original 32-row SRAM footprint.
  Row copies respect LVGL's draw-buffer stride. DMA completion waits are bounded;
  a timed-out transfer retains ownership of its staging buffer until completion.
  The panel
  remains off until the initial system frame is written. Brightness and idle
  dimming use the panel command rather than an LEDC backlight.
  UI/NVS brightness 2–100% maps linearly to panel brightness 10–100%, with
  integer rounding. Idle dimming uses the same mapping so the lowest visible
  setting does not look like a powered-off display. Explicit 0% remains 0%;
  idle screen-off still uses the panel's display-off command. Logs show both
  logical and panel brightness. Existing saved brightness values need no migration.
- CST92xx: shared I2C, address `0x5a`, interrupt GPIO6, native touch coordinates.
  A dedicated priority-6 task polls and acknowledges reports every 10 ms,
  independent of interrupt edges. LVGL drains a bounded queue for each contact,
  never I2C directly. Press/release positions survive delayed reads; consecutive
  motion samples coalesce. I2C transaction timeouts are bounded at 20 ms.
  Sample age is converted from ESP monotonic time into LVGL's tick domain.
  An unchanged held contact uses the current LVGL tick, keeping idle timers
  active. Passing boot timestamps directly makes unsigned idle time wrap and
  can cause immediate dimming/locking. Unlock resets idle time and restores
  brightness; `power ... touch_clock=lvgl` logs the resulting power state.
  Malformed reports and I2C failures do not abort; stale presses release after
  150 ms. Valid empty reports release immediately.
- ES8311: address `0x19`, MCLK 54, BCLK 37, WS 49, DOUT 52 and amplifier enable 45.
  Playback uses the PXA Host's 16 kHz rate, with mono samples duplicated into
  stereo I2S slots. Six 320-frame DMA blocks match the Host's 20 ms output
  blocks, keeping normal writes from leaving partially filled DMA blocks.
  Blocking I2S writes pace the Host output task directly, without an additional
  20 ms software timer. This fills the existing DMA ring ahead of playback to
  tolerate scheduling jitter and avoid drift between software and I2S clocks.
  The five writable blocks can queue up to about 100 ms of audio; no additional
  buffering memory is allocated.
  The codec uses the official BSP's `no_dac_ref=true` routing and 5 V PA /
  3.3 V DAC gain calibration. Silent preroll precedes PA enable and unmute.
  Codec I2C control has a separate mutex from PCM writes, so volume changes
  and register diagnostics cannot hold up audio DMA refill on an I2C timeout.
  I2S DMA interrupts remain active while the flash cache is disabled. Writes
  check the driver's byte count with a 100 ms timeout; a failed or short write
  restarts and silently preloads DMA, then retries the complete PCM frame once.
  Persistent failures are reported to the Host. DMA diagnostics count actual
  completed blocks and blocks containing nonzero PCM, plus successful restarts.
  Continuous nonzero Host output without any nonzero DMA progress for 22
  writes (about 440 ms, beyond the six-block preroll) also restarts the channel;
  idle silence and normal short effects do not trigger this check.
  Microphone capture is not exposed.
  Five-second `mosaico_audio` diagnostics show initialization status, UI/codec
  volume, PA pin readback, successful writes, nonzero PCM blocks, interval peak,
  write errors, and codec registers 0x31 (mute), 0x32 (volume), 0x44 (reference).
  A separate music line reports cumulative underruns and decode/read latency.
  Increasing `nonzero`/`writes` with `pa=1` and unmuted register 0x31 locates a
  silent-speaker problem after Host mixing; stalled counts or growing underruns
  point to the producer/output pipeline. Initialization failures retain their
  failing stage/code for later logs, after USB CDC becomes available.
- Peripheral power: GPIO60 is active low with a 50 ms soft ramp. GPIO57 remains
  released as an open-drain output; software does not trigger physical shutdown.
- GPIO7: single click wakes an idle display or returns home; holding for
  1.2 seconds starts Wi-Fi AP provisioning with the `ESP-Mosaico` SSID prefix.
- Native USB OTG: TinyUSB CDC ACM interface 0 carries console logs and PXADB.
  UART and USB Serial/JTAG transports on existing boards are unchanged.
- PXA Surface: 1× / 2× / 4× integer scaling and RGB565 / premultiplied ARGB composition,
  preserving opaque trusted UI and both alpha overlays. Frame capture reads the
  completed scanout buffer, converting its wire byte order to RGB565LE on demand.

## Diagnostics and GameRender

PXADB advertises `input-v2`, `screenshot-rgb565` and `screenshot-jpeg` when
development controls are enabled. Captures include guest surfaces, trusted
UI, alpha overlays and the performance overlay, and copy only a completed
panel frame. Metadata records its frame ID, completion time and source.
`--after-present` redraws a static screen before capturing; failed or incomplete
transfers are not returned as valid screenshots.

```sh
python3 tools/pxadb/pxadb.py screenshot screen.png --port /dev/ttyACM0
python3 tools/pxadb/pxadb.py screenshot screen.jpg --jpeg --after-present --port /dev/ttyACM0
python3 tools/pxadb/pxadb.py input key home --port /dev/ttyACM0
```

The GUI can use the same screenshot and remote input capabilities. Disconnect
its serial connection before running CLI commands or flashing; two serial
readers can consume each other's logs and protocol responses.

GameRender supports native 480 × 480, 240 × 240 at 2×, and 120 × 120 at 4×.
Opaque RGB565 frames covering the complete unlocked screen with no trusted UI
regions bypass LVGL redraw and go directly through the same DMA
transfer/completed-frame capture path. Complete guest/system alpha planes can
also use this path: PPA converts the game, then the CPU blends only the clipped
overlay intersections. Opaque trusted regions, missing overlay snapshots,
offsets, smaller surfaces and premultiplied ARGB frames use LVGL composition.

A raster task at priority 4 on core 1 and its helper on core 0 draw outside
LVGL's UI lock. The presenter at priority 5 on core 1 acquires only completed
frames, allowing PPA conversion of one frame to overlap CPU rasterization of
the next. The existing three application buffers preserve immutable PPA
input; two-buffer applications safely wait for a released lease. A global
raster guard also protects the shared helper when applications are replaced.
Only presentation/composition holds the UI lock, reducing touch blocking.
The flush worker runs at priority 6 on core 0, sleeping during TE and DMA
waits so rendering and audio can use the CPU. This adds one 4 KiB task stack;
it allocates no additional framebuffers.
PPA fills validated full-width GameRender background bands before the CPU
rasterizes the remaining commands, as on Korvo. Eligible full-panel RGB565
frames use a blocking PPA SRM operation for copy, exact 1x/2x/4x scaling and
byte swapping into the owned PSRAM presentation buffer. This is a board
GameRender path; LVGL's drawing backend remains unchanged. Driver cache
synchronization completes before CPU/scanout reads. Allocation/operation
failures fall back to the CPU path: native composition byte-swaps pixel pairs;
2x/4x expansion writes pixel pairs and copies repeated rows.
RGB565 frames with transparent system/guest overlays first use the fast
base copy/scale path, then blend clipped overlay intersections, skipping zero
alpha and directly storing opaque colors. Hidden planes skip blending. Opaque
trusted regions and ARGB game surfaces retain the generic compositor.
UI-only refreshes over eligible fullscreen games prepare one complete game plus
alpha frame per LVGL refresh and exchange scanout buffers, avoiding the full
producer restore and snapshot copies for Back indicators, status bars and
volume controls. No extra framebuffer is allocated. Lock/Home/Recents visibility
and fresh-frame resume barriers also apply to this path. `surface_stats`
reports `alpha_frames` to identify accelerated overlay compositions.
Home/Recents gestures capture one current Surface RGB565 frame into an LVGL
image before scaling/translation. While that image or Recents owns output,
Surface raster/presenter notifications are suspended without changing the
application lifecycle or lock gates. Cancellation/completion/deletion releases
the image and resumes output; returning from Recents requires a fresh frame for
direct scanout. The first Back indicator sample includes its chevron and rounded
shape, avoiding the former arrowless dark rectangle during the first 36 pixels.
`bash tools/test-game-system-gestures.sh` exercises the actual LVGL renderer,
scaled game pixels, cancellation, Recents ownership and the first Back frame
using an existing esp-mosaico simulator build.
Smaller plain surfaces invalidate their own bounds and their previous
position when moved. Composition still refreshes the full screen; fullscreen
RGB565 games with valid alpha planes use the accelerated board compositor.
The QSPI clock is 48 MHz (ESP32-S31's 480 MHz BBPLL divided by 10), below
CO5300's 50 MHz write-clock limit. Ideal full-frame RGB565 wire time is 19.2 ms,
down from 23.04 ms at 40 MHz; command, copy and TE overhead remain additional.
Signal integrity and actual scrolling frame rate still require device verification.
At 48 MHz a full panel update already needs 19.2 ms on the wire. Waiting for a
fresh approximately 16.95 ms TE cycle for each full update normally limits
scan-follow output to approximately 29.5 FPS. Korvo's RGB scanout/buffer-switch
path and Pai's smaller 296 x 240 panel have different limits. Native game
resolution remains 480 x 480; this optimization does not force lower quality.
This display's LVGL refresh timer targets 33 ms
(approximately 30 FPS); other boards keep their own refresh periods. Flush
callbacks merge dirty rectangles into a persistent RGB565 PSRAM frame and
return LVGL draw buffers as soon as their pixels have been copied. At the end
of a refresh, the affected rows are copied into a separate immutable scanout
snapshot. Full direct game frames instead exchange producer/scanout buffers,
eliminating the 460,800-byte snapshot copy. The first partial UI refresh after
a direct frame restores the producer once before merging its dirty rectangles.
The worker transfers that snapshot while LVGL renders the next frame.
At most one snapshot is submitted; producers sleep until it is available again,
bounding latency without dropping partial updates. The affected vertical
span as complete 480-pixel rows, preserving pixels outside the dirty rectangles.
This removes rendering gaps between panel writes and gives a consistent row
write speed. One CASET/RASET window covers the refresh: RAMWR starts the first
band and RAMWRC continues subsequent bands, avoiding window commands per band.
The persistent frame reuses the direct-presentation buffer; the snapshot adds
460,800 bytes in PSRAM at initialization. Removing the separate screenshot
shadow saves 460,800 bytes in PSRAM. DMA SRAM usage is unchanged.
Screenshots, brightness commands, direct presentation and initial panel enable
are synchronized with the worker. Health logs report actual completed-frame
`fps` and `direct` frame counts. Static screens intentionally report near-zero
FPS. A measured panel period near 17 ms allows about 29.5 FPS when complete
refreshes take two scan cycles; actual scrolling and games require measurement.
The presenter blocks on the adapter lock while startup builds the reference UI,
instead of logging a lock timeout every 100 ms during normal initialization.

## Small-screen display profile

The 480 x 480 panel is approximately 40 x 40 mm (305 DPI). The board reports
that density and 12-pixel safe insets, retaining the 58-pixel corner radii.
The reference UI increases text and spacing to 150%: body 24 px, labels 21 px,
captions 18 px, launcher icons 75 px, settings rows at least 84 px (about 7 mm).
Home uses a 3-column x 2-row grid with paging. Native application and GameRender
resolution remains 480 x 480. The UI simulator accepts `--density-dpi`, and
its `esp-mosaico` profile shares the panel density and insets.
Settings scroll viewports reach the safe horizontal edges while text and cards
retain their padding. At 305 DPI the vertical scrollbar is 3 px wide and 15 px
from the screen edge; dialog scrollbars sit 3 px inside their own panel edges.

Wi-Fi uses the Watcher's bounded pools (4 static RX, 6 static TX, 6 dynamic
RX, RX BA window 6). LVGL allocations and LittleFS buffers prefer PSRAM so
internal SRAM remains available for DMA, audio and PXADB's 20 KiB filesystem
task stack. A PXADB switch that immediately returns to off can indicate a
service startup failure; inspect the USB console for `PXADB startup failed`,
`PXADB control task allocation failed`, and the health log's `sram` value.
App installation updates only the app package; these changes require flashing
the rebuilt board firmware before retrying `tools/dev.sh device`.

TE is connected to GPIO43. CO5300 mode 0 drives TE high during vertical blanking.
Both edges are observed to measure the blanking duration; refresh waits for a
fresh falling edge, then for the panel to scan the first affected row. At 48 MHz,
writing complete rows is slower than the approximately 60 Hz panel scan. The
writer follows the current scan and must finish before the following scan
reaches those rows. Starting this slow writer at TE rising can instead put new
pixels at the top and old pixels at the bottom, producing a stable tear line.
Missing pulses use a bounded timeout and reduced retry rate.

Health logs include `te_blank_us`, `tx_us`, `tx_max_us`, and `scan_overruns`.
A separate `frame_stats` line includes `target_fps=30`, `pipeline=async`,
`render_us`, `render_max_us`, `present_wait_us`, `te_wait_us` and `time_valid`,
so profiling fields fit PXADB's 320-byte log payload. Rendering duration excludes time
waiting for the previous snapshot; transfer duration excludes TE waiting.
`surface_stats` separates direct raster/acquire, compose and snapshot-copy
time, and counts PPA/CPU direct frames and accelerated background fills.
GameRender presentation telemetry is recorded at actual transfer completion,
with the raster-ready timestamp carried through the flush queue.
Overruns indicate that a completed
transfer missed its predicted next-scan deadline (with a 250 us margin); they
need investigation under load. This policy assumes the default top-to-bottom
scan and does not guarantee no tearing during scheduling or DMA stalls. Host
tests validate refresh batching and the timing model; actual motion and load
still require device verification.

CST92xx exposes two touch contacts. Two LVGL pointer devices retain the native
contact IDs across report reordering and partial release, and the Guest UI/
GameRender pointer adapter delivers both contacts independently. Gestures use
the same stable snapshot. Stale data is released after 150 ms.

System status updates wait until held pointers, scrolling and notification
animations finish before rebuilding controls. This preserves the object that
owns a drag while Wi-Fi signal, volume or clock samples change.

SNTP starts after Wi-Fi obtains connectivity and restarts on reconnection.
Synchronization updates UTC epoch time; the board displays its date and local
clock in UTC+8 (`CST-8`), matching its Chinese/Singapore profile. Status remains
invalid until the system clock is at least 2020. A successful sync logs
`System time synchronized`; status refresh publishes the date/time to the bars
and lock screen without waiting on the network in the UI task.

Audio uses the shared Host mixer and ES8311 at 16 kHz with I2S DMA. Startup keeps
the PA disabled, primes DMA with silence and unmutes after the codec settles.
Volume is capped at 95%. Jump Jump 3D uses Host Audio 0.8 prepared tracks and
streamed music; its sampling clock no longer depends on rendering. Prepared
long effects use a 1.5 MiB PSRAM asset-cache ceiling inside the shared 2 MiB
resource budget, rather than allocating that capacity at startup.

The 16 MiB NOR flash layout reserves 4 MiB for the factory firmware and
`0xBF0000` bytes for the `pxa_data` LittleFS partition. SPI NAND, fuel gauge,
IMU, magnetometer, status LED, haptic motor and light sleep
are not exposed by this initial port. The board does not advertise battery data.

## Build and use

Run from the workspace root:

```sh
. "$HOME/esp/esp-idf/esp-idf-main/export.sh"
tools/firmware.sh esp-mosaico build
tools/firmware.sh esp-mosaico flash --port /dev/ttyACM0
tools/firmware.sh esp-mosaico monitor --port /dev/ttyACM0
```

With an external UART bridge, use `--port /dev/ttyUSB0`. The firmware wrapper
defaults Mosaico flashing to 2,000,000 baud; `--baud` or `ESPBAUD` overrides it.
The S31 download stub enables compressed writes. On the tested board/bridge,
a 3.83 MB application took 30.2 s instead of 73.1 s with ROM-only writes at the
same baud. Existing configurations should disable `ESPTOOLPY_NO_STUB` as well.
Without an automatic reset circuit, enter download mode manually while the
serial connection is open, and reset manually after a verified write.

The build directory is `build/firmware/esp-mosaico`. The generated configuration
and dependency resolution are isolated in `firmware/sdkconfig.esp-mosaico` and
`firmware/dependencies.lock.esp-mosaico`. No reference checkout is required to build.

`tools/firmware.sh` runs the component manager through
`tools/idf_component_registry.py`. If the public download index has no version
matching a dependency, it checks the official registry API before failing.
This handles releases such as adapter 0.7.2 appearing in the API while the
download index still lists 0.7.0. Version constraints, component hashes and
download checksum validation remain enforced. Custom storage mirrors and an
explicit `IDF_COMPONENT_WRAPPER` retain their existing behavior. For direct
`idf.py` builds, set `IDF_COMPONENT_WRAPPER` to the wrapper's absolute path.
On 2026-10-08 the public adapter 0.7.2 archive and checksums URLs also returned
HTTP 403. This machine builds successfully using its validated Component Manager
cache. API fallback fixes version resolution; downloading with an empty cache
still requires the public archive to become available.

If the device already runs this firmware, fully power it off, hold BOOT, power
it on again, then release BOOT after USB enumerates. A battery-powered board
does not power off just because its USB cable is unplugged. Stop serial monitors
and check the port again after entering download mode; its ttyACM number can
change. This USB CDC implementation does not implement esptool's automatic
download-reset sequence, so this board defaults to `--before=no-reset` to keep
the manually established download-mode connection intact.

If esptool prints `Connecting` and subsequently reports `Permission denied`,
the first serial open succeeded; investigate access after USB re-enumeration
rather than assuming a permanent lack of permissions. On Linux the current
terminal session needs the device's group (usually `uucp` on Arch Linux or
`dialout` on Debian/Ubuntu). Verify `id` and `ls -l /dev/ttyACM*`; after adding
a group, log out and back in. Avoid a one-off `chmod` workaround because a USB
reset creates a new device node. The connection handshake uses 115200 baud, so
increasing `--baud` cannot fix a failure at `Connecting`.
Standard output and error are unbuffered after USB console redirection so
monitor logs are sent immediately instead of waiting for a stdio buffer to fill.

Build the font-only factory image separately, then flash it when needed:

```sh
tools/factory.sh image esp-mosaico
tools/app.sh build <app-id> --board esp-mosaico --source-root local/pxa-apps
tools/dev.sh device <app-id> --board esp-mosaico --port /dev/ttyACM0
```

Firmware flashing does not populate or erase `pxa_data`. App sources and the
system font must be supplied locally as described in the workspace README.
The matching simulator profile is `esp-mosaico` (480 × 480).

Compile and host tests do not establish hardware correctness. Verify revision
detection, cold boot, USB reconnect, panel colors, edge touch coordinates,
speaker playback, Wi-Fi provisioning, trusted overlays and idle wake on device.

Host regressions (with AddressSanitizer and UndefinedBehaviorSanitizer):

```sh
bash firmware/boards/esp-mosaico/tests/test_host.sh
python3 -m unittest discover -s tools -p 'test_dev.py'
```

Touch gestures retain controller tracking IDs and last coordinates when sending
release events, including partial two-finger releases and stale-report releases.
Unused slots are not submitted as synthetic releases for tracking ID zero.

Runtime health is logged every five seconds (`mosaico_hw`): reset reason,
uptime, completed refreshes, SPI errors, touch read/report/error counts,
active touch coordinates, TE edge/period/timeout metrics and internal
free memory. Read it with `python3 tools/pxadb/pxadb.py logcat --port /dev/ttyACM0`.
