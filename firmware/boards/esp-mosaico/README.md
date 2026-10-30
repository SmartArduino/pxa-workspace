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

TLS record and certificate allocations use PSRAM
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`). Store installation opens a separate
HTTPS connection after the catalog/detail request; restricting both TLS
connections to internal SRAM can exhaust the DMA heap during the handshake.
Certificate verification remains enabled. Existing local sdkconfig files must
select the same allocation strategy when upgrading this board configuration.

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
  One retained 480-row PSRAM draw buffer uses LVGL DIRECT rendering with
  4-column / 2-row-aligned dirty areas. Its capacity equals the previous two
  240-row scratch buffers; it eliminates their copy into a separate producer.
  A dedicated flush task sends aligned, contiguous snapshot rows directly
  from PSRAM using the same SPI DMA mode as DOIT GameKit 1. The LCD IO driver
  splits large writes at the SPI transaction limit and signals the final chunk.
  Narrow strided windows retain two 16-row internal staging buffers;
  `PXA_MOSAICO_LCD_PSRAM_DMA_DIRECT=n` uses that path throughout.
  LVGL renders its retained UI frame while SPI reads the immutable snapshot.
  Snapshot exchange, UI copying and idle diagnostics wait for pending DMA,
  including recovery from a timeout; a failed direct transfer is not retried
  through staging while it can still own its source. Completed scanout is converted to
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
  A dedicated priority-6 task polls and acknowledges reports on a 10 ms cadence,
  independent of interrupt edges. The same worker polls battery status every
  two seconds with bounded I2C timeouts. LVGL drains a bounded queue for each contact,
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
  Microphone capture uses the standard Audio capture API: 16 kHz mono,
  20 ms raw Opus frames, encoded on a dedicated worker with a four-packet queue.
  ADC capture opens on demand; revocation/closure stops delivery and closes it.
  Playback and capture use separate ES8311 DAC/ADC control objects and shared
  full-duplex I²S (DOUT52, DIN40). Encoding uses the shared Device audio budget.
  Five-second `mosaico_audio` diagnostics show initialization/stopped status,
  logical volume, PA pin readback, successful writes, nonzero PCM blocks,
  interval peak and write errors, without codec I2C reads. A separate music
  line reports cumulative underruns and decode/read latency; a busy mixer skips
  that snapshot instead of blocking the UI. Codec registers 0x31 (mute),
  0x32 (volume) and 0x44 (reference) are read once during initialization and
  logged as `codec_boot`; these values describe startup, not live codec state.
  Increasing DMA `nonzero` with `pa=1` locates a silent-speaker problem after
  Host mixing; stalled counts or growing underruns point to the producer/output
  pipeline. Initialization failures retain their failing stage/code for later
  logs, after USB CDC becomes available.
- BQ27220: address `0x55`, read-only battery percentage and charging state.
  Status/UI read a cached snapshot, without I2C on the LVGL thread. Failed or
  invalid samples expire after ten seconds; missing hardware is not advertised.
  The driver does not change the fuel-gauge profile, unseal it or write data flash.
- Peripheral power: GPIO60 is active low with a 50 ms soft ramp. GPIO57 is an
  open-drain shutdown request to SAM8108, released during normal operation.
  Confirmed shutdown stops Guest activity, waits for save/detach acknowledgement,
  turns off the screen/amplifier and holds GPIO57 low. Restart uses the same
  asynchronous Guest shutdown path. Missing acknowledgement cancels the
  physical power action and restores input/the power menu for retry. Audio
  shutdown waits for any current write/recovery before disabling DMA and PA;
  later callbacks discard audio while preserving the output cadence. Both
  actions reuse the existing input worker; no extra task or stack is allocated.
  PXADB advertises board power-off support.
- GPIO7: single click wakes an idle display or returns home; double click opens
  the system power menu (restart/shutdown); holding for
  1.2 seconds starts Wi-Fi AP provisioning with the `ESP-Mosaico` SSID prefix.
- Native USB OTG: TinyUSB CDC ACM interface 0 carries console logs and PXADB.
  UART and USB Serial/JTAG transports on existing boards are unchanged.
- PXA Surface: 1× / 2× / 4× integer scaling and RGB565 / premultiplied ARGB composition,
  preserving opaque trusted UI and both alpha overlays. Frame capture reads the
  completed scanout buffer, converting its wire byte order to RGB565LE on demand.

### Onboard NAND, motion sensors and vibration

The extra GD5F1GM7UEYIGR is a 1-Gbit (128-MiB raw) SPI NAND, separate from
the boot NOR flash. SPI3 uses CLK 20, MOSI 21, MISO 22, CS 23, HOLD/IO3 24
and WP/IO2 25, in QIO mode at 20 MHz. Initialization first holds GPIO24/25
high and uses single-line SPI to read the chip ID and enable/verify the Quad
Enable bit while preserving the other configuration bits. Only then are IO2/IO3
routed to SPI3. This also works after a warm restart with QE already enabled.
The bootstrap follows the Apache-2.0 ESP-Mosaico BSP; its source revision and
license are retained under `vendor/esp-mosaico-bsp/`.
The LCD keeps SPI2. The pinned
`espressif/spi_nand_flash 1.4.4` (Apache-2.0) and `espressif/dhara 1.1.0`
(ISC) provide ECC handling, bad-block management and wear leveling below
ESP-IDF's FAT block-device mount. Usable space is smaller than raw capacity.
Their upstream license files remain in the managed component packages.

The NAND mounts at `/pxa/nand`, alongside the existing `/pxa` LittleFS volume,
and appears as **NAND storage** in PXA's volume list and installation picker.
It supports applications, private data through PXA's FAT image backend, and
resources. Successful registration selects it as the default app-media volume.
Its fixed device identity includes the MCU MAC and onboard wiring identity;
the persistent volume UUID remains in the filesystem. An unavailable NAND
does not prevent startup with internal storage.

Private key/value reads and writes run on a bounded Host storage worker. The
runtime owns request admission and completion; the worker receives copied
keys/values and serializes the existing durable snapshot backend. A successful
save still means the disk write completed. Activation shutdown drains accepted
operations before releasing the backend or private filesystem, so gameplay
checkpoints do not require synchronous NAND writes on the Guest event loop.

Startup automatically initializes a FAT filesystem when a read-only probe
returns `FR_NO_FILESYSTEM`. This is enabled by default through **ESP-Mosaico
peripherals → Automatically initialize onboard NAND without a filesystem**
(`CONFIG_PXA_MOSAICO_NAND_FORMAT_IF_MOUNT_FAILED`). Existing valid FAT volumes
mount directly. I/O failures and other probe errors leave the NAND unavailable
for that boot and do not trigger formatting. If the probe cannot detach, its
FatFS/BDL resources remain owned until reboot.
Disable the setting to preserve content that PXA does not recognize as FAT;
automatic initialization erases that content rather than migrating it.
Board defaults enable BDL and FAT sectors up to 4096 bytes, long filenames
and 16 file locks; merge those settings into older local sdkconfigs.
Failed SPI cleanup retains ownership; a repeated initialization attempt first
finishes that cleanup before creating a new bus/device. A failed FAT unmount
retains its live BDL and prevents reinitialization until reboot.

BMI270 at I2C address `0x69` exposes `motion.acceleration` (X/Y/Z, micrometres/s²),
`motion.angular-velocity` (X/Y/Z, microradians/s) and `imu-temperature` (millidegrees C).
The IMU uses ±4 g and ±2000 degrees/s at 100 Hz. Temperature is the chip's
die temperature. Both motion channels are converted by the board Provider
before Sensor delivery: device `(X,Y,Z) = (chip Y,-chip X,chip Z)`.
With the panel in its normal orientation, device +X points toward the top
edge, +Y toward the left edge and +Z out of the screen. This is a right-handed
frame; the same proper rotation applies to acceleration and angular velocity.
Screen-up stationary acceleration is approximately +9.81 m/s² on Z;
screen-down is approximately -9.81 m/s². Angular velocity follows the right-hand
rule, so counterclockwise rotation viewed from the screen side is positive Z.
UI pixel Y increasing downward does not change this physical coordinate frame.
Applications must not add ESP-MOSAICO-specific mounting compensation.
Two independently detected BMM150s at `0x11` and `0x12`
expose `magnetic-field.0` and `magnetic-field.1` in nanotesla at 10 Hz, using
Bosch's factory trim compensation (integer output has 1-microtesla steps).
Magnetometer axes remain in each chip's native frame;
these values are not a calibrated compass heading. The motor can affect
magnetic readings. Missing or failed chips are omitted from enumeration.

Applications discover channels through Sensor service 8 and subscribe with
`sensor.read` permission scoped to the exact semantic name. Minimum periods
are 10 ms for motion and 100 ms for temperature/magnetometers; maximum is
60 s. The Host polls only while subscribed, including native main-loop apps.
The IMU and each magnetometer enter standby after their last subscription
closes. I2C failures and invalid compensation results produce no sample.
The original Bosch SensorAPI files are vendored unchanged under `vendor/`,
with BSD-3-Clause licenses, source commits and per-file SHA-256 manifests.

The GPIO8 motor has bounded on/off control: `EspMosaicoVibrate(duration_ms)`
accepts 0–1000 ms; zero stops immediately, and a new pulse replaces the old
deadline. Initialization, lock and orderly shutdown leave the motor off.
Applications use standard **Haptics service 23** through `ctx.haptics()`, with
`haptics.play` permission scoped to `device.default`. Capabilities advertise
binary vibration and a 1000 ms maximum; only amplitude 1000 is supported. The
exclusive actuator handle stops on closure, revocation, foreground loss and
application exit. The board deadline timer remains independent of Guest code.
See the [standard Haptics contract](../../../deps/pxa-system/spec/draft/haptics.md).

The BQ27220 cache also exposes Sensor channels `battery-voltage` (microvolts),
`battery-current` (signed microamperes, positive charging) and
`battery-temperature` (millidegrees C). These sample every 2 s; Host reads only
cached snapshots, never the I²C bus. Invalid/stale readings produce no sample.
All eight sensor channels may be subscribed concurrently. The fuel gauge is
read only; no unseal, profile update or calibration writes are performed.

### Mainboard controls and general expansion

BOOT (GPIO61, active low) acts as Back while awake/unlocked and wakes the screen
while asleep/locked. It uses the same trusted navigation path as other board
controls. AI remains GPIO7. The status LED exists only on revision 1.0 (GPIO3,
active low) and is exposed as logical `board.status-led` on that revision.

The optional [peripherals extension](../../../extensions/peripherals/README.md)
provides the following logical ports. Applications enumerate names and use
`org.doit.peripherals` permission scoped to the selected name; they do not pass
ESP32 GPIO numbers to the Host.

| Logical port | Board connection / behavior |
|---|---|
| `left.slot`, `right.slot` | Read-only cached EEPROM identity/CRC and an exclusive slot claim |
| `expansion.i2c` | SDA0/SCL1, 10–400 kHz, 7-bit address, asynchronous transactions up to 256 bytes each way |
| `right.uart` | UART1 on TX58/RX59, 1200–921600 baud, 8N1; available only after USB console redirection succeeds |
| `left.gpio1`…`left.gpio12` | GPIO53,48,13,12,14,4,55,19,18,17,16,15; input or logical output |
| `right.gpio1`…`right.gpio7` | GPIO46,47,11,10,39,5,38; input or logical output |
| `board.status-led` | Revision 1.0 only; 0=off, 1=on |

Revision 1.0 shares the mainboard I²C bus; its onboard touch, codec, motion,
magnetometer and gauge addresses are reserved. Revisions 1.1/1.2 use independent
I²C1 for expansion. EEPROM addresses 0x50/0x51 are always reserved. Board-owned
LCD/NAND/audio/power/sensor/USB pins are excluded from generic GPIO. In
particular right-side I²S pins and left-side USJ pins 33/34 stay reserved.
UART0 ROM boot output can still appear on pins58/59 before USB initialization.

A component may claim multiple GPIOs on the same side; that side is exclusive
against other components. Claiming a GPIO/UART/slot suspends that side's module
scanner. Closing the last claim safely resets outputs and restores the slot
selector (left GPIO14 low, right GPIO39 high) before probing resumes. I²C device
addresses are exclusive. Transfer timeouts include wire time at the selected
clock rate (at most 485 ms for a maximum-size 10 kHz transaction). On revision
1.0, an atomic external transaction occupies the shared bus for its duration;
keep transfers short when touch/motion latency matters. Cleanup retains
ownership on native driver errors. Once peripheral shutdown starts, GPIO/UART
I/O and new I²C transfers return Unavailable immediately. Worker notifications are drained
before task deletion; late close/cancel callbacks cannot wake a deleted task.

Module presence is debounced across three 250 ms probes; EEPROM metadata is
revalidated every 2 s while unclaimed. Reads are split into short 16-byte chunks
so mainboard traffic can interleave on revision 1.0. Descriptor, manufacturing
and parameter CRCs are checked; the Host never writes the EEPROM. Cached
presence/valid/claimed/unknown/error flags distinguish unavailable information
from a valid module. A transient EEPROM device-creation failure is retried
at 2 s intervals. Bus probe errors restart debounce; three consecutive healthy
probes are required to confirm presence/absence and recover the cached state.
Failed native opens retain their pins/address while worker-side rollback runs;
retries return Busy until that cleanup and slot-selector restoration finish.
GPIO/I²C/UART driver preparation runs on the expansion worker. Host callbacks
only reserve metadata or read cached status, so a slow driver setup does not
hold the Host's resource mutex or event loop. An OPEN response is delivered
only after preparation succeeds. Pending opens remain bound to their permission
and are cancelled on revocation, foreground loss or application exit.

The worker sleeps until the next 250 ms module-scan deadline while discovery
is needed, retries incomplete cleanup at 20 ms, and waits for notifications
without a periodic timeout when both slots are claimed and no work remains.
Queued opens, transfers, closes and shutdown wake it immediately. This reduces
idle worker wakeups; device power consumption still requires measurement.
Module-specific camera and accessory functionality is not
part of this generic port layer.

Run `tests/test_peripherals.sh` for sanitizer-enabled hardware-emulation tests.
Logs and generated binaries belong under `local/artifacts/`.
Hardware references: [Espressif user guide](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s31/esp-mosaico/user_guide.html)
and [CoreBoard V1.2 schematic](https://dl.espressif.com/AE/Mosaico/SCH_SCH_ESP-Mosaico_CoreBoard_V1_2_1_2026-09-24.pdf).

## Diagnostics and GameRender

PXADB advertises `input-v2` with two pointers, `screenshot-rgb565` and `screenshot-jpeg` when
development controls are enabled. Captures include guest surfaces, trusted
UI, alpha overlays and the performance overlay, and copy only a completed
panel frame. Metadata records its frame ID, completion time and source.
`--after-present` redraws a static screen before capturing; failed or incomplete
transfers are not returned as valid screenshots.
Remote input uses the same two LVGL pointer devices as physical input, rather
than creating another pointer. Queues retain short press/release endpoints;
cancelling injection suppresses clicks. A real touch takes ownership back and
notifies PXADB, so a continuing remote gesture cannot compete with it. Late
remote presses are rejected while physical contacts remain down; late remote
releases do not release those contacts. Queue overflow cancels the old gesture
explicitly, preventing a synthetic release from clicking its old target.
Logical pointers and battery polling remain available if CST92xx fails to probe.

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
LVGL's UI lock. The guest runtime runs on core 0, independently of the UI and
presenter on core 1. The presenter coalesces adjacent pixel/text submissions
for one scheduler tick before acquiring the UI lock. It acquires only completed
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
frames at 1x use a blocking PPA SRM operation for copy and byte swapping
into the owned PSRAM presentation buffer. 2x/4x expansion uses CPU nearest
neighbour copies: SRM upscaling changed edge colors on this S31, so it is
not used for pixel art. LVGL primitives stay on the software drawing backend. Driver cache
synchronization completes before CPU/scanout reads. Allocation/operation
failures fall back to the CPU path: native composition byte-swaps pixel pairs;
2x/4x expansion uses fixed-scale paired reads/writes, combining byte swapping
with replication without per-pixel division, then copies repeated rows.
RGB565 frames with transparent system/guest overlays first use the fast
base copy/scale path, then blend clipped overlay intersections, skipping zero
alpha and directly storing opaque colors. Hidden planes skip blending. Opaque
trusted regions and ARGB game surfaces retain the generic compositor.
After DMA has released the old snapshot, complete direct frames are compared
in panel byte order. Only the aligned changed window is sent; identical frames
use one minimal window for normal completion. Snapshots remain complete for
capture and later UI restoration. Diagnostics are painted into each new snapshot
and merge their own changed region, including when the overlay is disabled.
The direct presenter resolves layout and acknowledges LVGL invalidations only
after a complete eligible frame has been queued. The same gate runs at refresh
start if LVGL obtains the lock first. This prevents an additional full software
redraw of the same game/alpha frame. Failed presentation retains ordinary LVGL
refreshes; acknowledging a frame does not cancel subsequent animation updates.
The pinned LVGL's internal invalidation-queue operation is isolated in
`esp_mosaico_refresh.h` and covered
by the real-LVGL refresh tests; check this adapter when upgrading LVGL.
UI-only refreshes over eligible fullscreen games prepare one complete game plus
alpha frame per LVGL refresh and exchange scanout buffers, avoiding the full
producer restore and snapshot copies for Back indicators, status bars and
volume controls. No extra framebuffer is allocated. Lock/Home/Recents visibility
and fresh-frame resume barriers also apply to this path. `surface_stats`
reports `alpha_frames` to identify accelerated overlay compositions.
The initial lock state, screen-off and wake state are published to both the
Host and Surface visibility gate. The power menu has its own overlay gate,
so direct games cannot draw over it. Pending Home/Back/wake controls use the
existing 16-ms display timer; network/status sampling retains its 100-ms timer.
Fullscreen direct presentation also works with the optional reference UI disabled.
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
This display's LVGL refresh timer is 16 ms; actual output is bounded by rendering,
TE and transfer completion, rather than this timer alone. Other boards keep
their own refresh periods. One retained full-height draw buffer keeps the
460,800-byte (450 KiB) PSRAM saving versus the original two full-height buffers.
LVGL renders dirty rectangles directly into that retained UI frame. Flush
callbacks collect their bounds without copying each chunk into another frame.
At the end of a refresh, narrow dirty rectangles are copied into a separate immutable
scanout snapshot. Wide rectangles (at least 75% of the screen width) retain
full-row copies to avoid per-row call overhead. LVGL draws native RGB565;
large full-row snapshots reuse the existing PPA SRM client for one 1x copy/byte
swap; narrow/small snapshots or accelerator failures use a paired CPU conversion.
SRM invalidates whole output rows, so narrow blocks stay on the CPU to preserve
untouched pixels which may still be dirty in the cache.
This avoids repeated byte-order work during text and opacity drawing without
adding a framebuffer or enabling per-primitive PPA rendering. One software
draw worker avoids the measured cross-core overhead and saves about 32 KiB
of SRAM task storage compared with the former two-worker configuration. Full direct game
frames instead exchange producer/scanout buffers,
eliminating the 460,800-byte snapshot copy. The first partial UI refresh after
a direct frame restores the retained UI frame before LVGL draws new patches.
Letterboxed RGB565 surfaces can also use direct composition when an immutable
full-screen Canvas plane is fully opaque outside the surface. The compositor
checks the matte's alpha coverage under the frame lease; missing, translucent
or suppressed coverage and opaque host regions retain the LVGL fallback.
Partial sources use CPU composition and never enter the full-frame PPA copy.
The worker transfers that snapshot while LVGL renders the next frame.
The final UI flush queues its retained source to the existing core-0 worker.
That worker converts the snapshot, calls `flush_ready`, publishes source
ownership and wakes the producer before starting TE/DMA. Input and layout can
advance during conversion; the single retained pixel buffer waits until its
source lease is returned. Both render-start and flush-wait guard that handoff,
including refreshes initiated by a different task such as PXADB. The request
still occupies 40 bytes and no new task, semaphore or frame buffer is allocated.
At most one snapshot is submitted; producers sleep until it is available again,
bounding latency without dropping partial updates. Small windows can transfer
only their aligned columns if a measured TE interval admits the entire update
between scans. The worker prefers a window before the first row is scanned;
otherwise it waits until the last row was scanned. Wire-time, per-band overhead
and scheduling margins are budgeted, and a delayed worker rechecks admission.
Tall/wide updates, unknown TE timing and missing pulses retain full-row
scan-follow transfers. A compact transfer that misses its deadline disables
compact mode until reboot; dirty snapshot copying still works. No extra frame
cache or pending-frame queue is allocated. One CASET/RASET window covers the refresh: RAMWR starts the first
band and RAMWRC continues subsequent bands, avoiding window commands per band.
The retained UI frame, direct-game producer and scanout snapshot each occupy
460,800 bytes in PSRAM. Their combined capacity is unchanged from the previous
two-half-buffer path. Removing the separate screenshot
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
fresh falling edge. Full-row transfers then wait for the panel to scan the first
affected row. At 48 MHz, writing complete rows is slower than the approximately 60 Hz panel scan. The
writer follows the current scan and must finish before the following scan
reaches those rows. Starting this slow writer at TE rising can instead put new
pixels at the top and old pixels at the bottom, producing a stable tear line.
Missing pulses use a bounded timeout and reduced retry rate.

Health logs include `te_blank_us`, `tx_us`, `tx_max_us`, and `scan_overruns`.
A separate `frame_stats` line includes `ui_period_ms=16`, `pipeline=async`,
`render_us`, `render_max_us`, `present_wait_us`, `te_wait_us` and `time_valid`,
so profiling fields fit PXADB's 320-byte log payload. Rendering duration excludes time
waiting for the previous snapshot; transfer duration excludes TE waiting.
`surface_stats` separates direct raster/acquire, compose and snapshot-copy
time, and counts PPA/CPU direct frames and accelerated background fills.
`ui_transfer` adds `compact_total`, `compact_enabled` and `draw_rows`.
The compact counter is cumulative since boot (32-bit wrap); `fps` counts panel
completions within the log interval. See the [UI optimization report](docs/ui-performance-optimization.zh-CN.md)
for the baseline, reproducible tests, transfer-volume comparison and device limits.
GameRender presentation telemetry is recorded at actual transfer completion,
with the raster-ready timestamp carried through the flush queue.
The top `FPS26.4` HUD counts successful content transfers and updates about once
a second. It is painted into the retained scanout by the existing transfer task,
so a scroll below the top edge still refreshes the value. Idle updates transfer
only the 60×16 diagnostic strip (rounded for the panel); those transfers do not
feed FPS or PXADB PERF. Turning the HUD off requests a clean UI redraw.
No additional task, framebuffer or queue is allocated. The shared HUD's decimal,
sampling, rollover and concurrency checks run with
`bash firmware/components/pxa_board_api/tests/test_host.sh` from the repository root.
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
`0xBF0000` bytes for the `pxa_data` LittleFS partition. SPI NAND,
IMU, magnetometer, status LED, haptic motor and light sleep
are not exposed by this port. See the [PXA integration audit](docs/pxa-integration-audit.zh-CN.md)
for connected interfaces, remaining hardware-specific work and validation limits.
Follow-up input, audio and power fixes are documented in the
[reliability and UI latency report](docs/reliability-and-ui-latency.zh-CN.md).
Further worker handoff measurements are in [the parallel pipeline report](docs/ui-parallel-pipeline-20261016.zh-CN.md).

Actual settings/shade measurements and the retained UI drawing path are recorded
in the [device profiling report](docs/ui-device-profile-20261016.zh-CN.md).

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

On Linux, `tools/firmware.sh` holds the native device's usbfs node read-only
before waiting for the build lock and running CMake/Ninja. This keeps runtime
USB autosuspend from interrupting the ROM while the build is busy, follows
re-enumeration at the same physical port, and leaves CDC/DTR/RTS and system power
settings untouched. Start the command before entering download mode so the
guard can acquire the new ROM immediately. A ROM which already stopped replying
after suspension may still need a fresh BOOT/reset; a visible tty node alone does
not prove that synchronization works. The suspend/reply failure is under device
verification; the guard's runtime-PM mechanism follows the Linux USB power API.

Ordinary firmware updates can use `app-flash` to preserve bootloader, partition
table, NVS and application data:

```sh
tools/firmware.sh esp-mosaico app-flash --port /dev/ttyACM2 --baud 2000000
```

ROM-only flashing is now the profile default because a rev0 USB-OTG stub transfer
previously failed midway. Existing saved configs must also set
`CONFIG_ESPTOOLPY_NO_STUB=y` (Serial flasher config → Disable download stub).
External UART can retain the faster stub path if separately verified. Stub
selection takes effect after synchronization; it does not repair an initial
`No serial data received` handshake failure. No full-chip erase is needed.

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

ESP32-S31 app packages use O3, the ILP32F ABI and the RV32IMAFCB instruction
set (`+m,+a,+f,+c,+zba,+zbb,+zbs`). Compressed instructions reduce instruction
fetches, while bit-manipulation instructions shorten address and bit operations.
WAMR's `--size-level=3` selects the Small code model; it does not select Os/Oz.
The bundled LLVM has no ESP32-S31 CPU model, so `generic-rv32` remains explicit.
Rebuild and reinstall the app package to apply these compiler settings; this
change does not require reflashing firmware. FPS gains require device measurement
and depend on how much time the app spends in Guest code versus Host rendering
and panel transfers.

Compile and host tests do not establish hardware correctness. Verify revision
detection, cold boot, USB reconnect, panel colors, edge touch coordinates,
speaker playback, Wi-Fi provisioning, trusted overlays and idle wake on device.

Host regressions (with AddressSanitizer and UndefinedBehaviorSanitizer):

```sh
bash firmware/boards/esp-mosaico/tests/test_host.sh
bash firmware/boards/esp-mosaico/tests/test_lvgl.sh
# Build a separate host LVGL library with the device's gesture recognizers enabled:
bash firmware/boards/esp-mosaico/tests/test_lvgl.sh --gestures
python3 -m unittest discover -s tools -p 'test_dev.py'
```

Touch gestures retain controller tracking IDs and last coordinates when sending
release events, including partial two-finger releases and stale-report releases.
Unused slots are not submitted as synthetic releases for tracking ID zero.

Runtime health is logged every five seconds (`mosaico_hw`): reset reason,
uptime, completed refreshes, SPI errors, touch read/report/error counts,
active touch coordinates, TE edge/period/timeout metrics and internal
free memory. Read it with `python3 tools/pxadb/pxadb.py logcat --port /dev/ttyACM0`.
