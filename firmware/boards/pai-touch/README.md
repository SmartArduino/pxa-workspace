# pai-touch Board

[简体中文](README.zh-CN.md)

This directory is one ESP-IDF component, selected with `PXA_BOARD=pai-touch`.
It contains the sample hardware implementation: JD9853 display setup, CST826
touch, ADC buttons, battery monitor, Wi-Fi and JL701 audio.

The speaker is driven by a JL701 coprocessor over UART; the board has no codec
of its own. `src/jl701_audio_link.cc` owns that transport (handshake,
keep-alive, PCM pacing and volume) behind the small API in
`include/jl701_audio_link.h`, `PaiTouchAudio` turns it into the PXA audio sink,
and the vendored RPC stack it builds on stays private to this component under
`vendor/rpc701/`. No shared component depends on it, so it is not part of
`firmware/components/`.

Behaviour measured on the device, which the link is built around:

- The coprocessor drops the link and sleeps once `keep_alive_ms` (4 s) passes
  without a heartbeat, so the link sends one every 2 s. To bring it back the
  link holds TX low for 1 s and re-runs the handshake; it does that once the
  link has stayed down, not on every retry.
- A handshake sent immediately after the wake pulse is lost while the
  coprocessor is still booting. The retry a few seconds later succeeds, which is
  why `start()` may report "still booting" and audio appears afterwards.
- Remote amplifier control stays disabled in the handshake. Asking the
  coprocessor to drive its own PA pin makes it stop answering entirely, and the
  vendor reference firmware leaves it disabled too; the speaker is audible
  either way.
- The coprocessor streams microphone Opus frames continuously, without flow
  control. Standard Audio capture exposes a bounded queue only while an
  application owns its capture resource. Opening/closing flushes stale frames;
  the nonblocking reader returns WouldBlock when empty. The installed JL701
  emits one 60 ms SILK/Opus frame even when 20 ms was requested, so the Host
  advertises the actual 60 ms wire format. Post-wake receive overflows are
  reported at most once every 10 s. Frames are not written to storage by the
  board driver.

The two CST826 contacts are exposed as independent LVGL pointer devices, so PXA
apps receive stable `pointer_id` values for simultaneous touches.

The top performance HUD updates about once a second. `FPS` counts completed LCD
transfers; `R` is the average rotation/conversion/composition time across ordinary
UI and direct game frames, and `M` is the average panel submission/completion time.
Both durations are truncated to whole milliseconds. `M` includes waiting for the
panel transfer, but excludes earlier queueing; neither value includes Guest update
or Host rasterization. Pipeline stages overlap, so `R + M` is not a frame budget.
FPS and HUD timing also work when compile-time performance logging is disabled.
Without new frames the last on-screen sample remains visible.

The launcher picks its target page on release and settles in 160 ms, without
an inertial coast followed by another snap. Short fast flicks also turn pages.
Icons and captions are vertically centered as one block. Delete buttons use a
40×40 pixel touch target around a 32 pixel circle on this display, and swipes
cancel deletion taps. Small displays omit icon shadows.
Clock and slider values update in place in the notification shade, keeping
status refreshes from rebuilding the launcher. Decoded PNG pixels use PSRAM
exclusively, with a 1 MiB image cache limit enforced during board initialization
even with a saved sdkconfig; the cache does not allocate its full budget upfront.
The data cache uses 64-byte lines while the instruction cache stays at 16 KiB
to preserve internal SRAM. Moving PXADB read staging, permission-list scratch
and store download records to PSRAM reduces internal static BSS by 10,992 bytes
in the ESP32-S3 linker map. Audio/Flash task stacks and ISR buffers remain internal.
For an existing `firmware/sdkconfig.pai-touch`, select 64-byte data cache lines
explicitly; defaults do not override saved options.

Launcher icons cancel activation once movement from the press origin exceeds
a DPI-scaled tap tolerance, including swipes that return to their origin or
cannot scroll farther at a page edge. Presses during page-settle animations
also suppress activation. Ordinary taps tolerate small jitter, and stationary
long presses still enter icon rearrangement.

After Wi-Fi obtains an IP address, the board synchronizes its system clock
with `pool.ntp.org`. The ESP-IDF SNTP service refreshes it periodically while
connected, and a later reconnection starts synchronization again.

Its `sdkconfig.defaults` and `partitions.csv` are loaded only for this board.
They define the `pxa_data` LittleFS partition, mounted at `/pxa`, separately
from firmware flashing. The defaults also enable LVGL's LodePNG decoder
because PXA Packages use PNG icons and UI assets. See
[PXA App Delivery](../../../docs/pxa-app-delivery.md) for development installation
and explicit factory-image creation.

Its `idf_component.yml` owns the board-only managed dependencies: CST826
touch support, buttons, battery estimation, Wi-Fi provisioning and FreeType.
The cross-board LVGL, LVGL port and LittleFS dependencies remain in
`firmware/main/idf_component.yml`.

`src/pai_touch_board_port.cc` is the only PXA integration sidecar. It maps the
existing hardware object to `pxa_board_port_t`; `parallel_sw_rotation_flush.h`
continues to own the direct PXA frame and SPI DMA path.

The component exports `pxa_board_register_selected()` in `include/pxa_board.h`;
`main` uses that stable name rather than a pai-touch-specific header.

## GC2145 camera

The MZ42-DJJXJ-S3-6824 V3.0 GC2145 is exposed as the optional workspace
`camera.jpeg` peripheral. Exact-scope permission and foreground ownership are
required. The camera initializes on open and deinitializes on close.

SCCB uses I2C0 on SDA0/SCL4, separate from the touch controller on I2C1.
D0–D7 are GPIO13,15,16,14,12,10,9,7. XCLK is GPIO8 at 32 MHz for 160×120 and 320×240, and 16 MHz for VGA,
PCLK11, HREF6 and VSYNC5. PWDN/reset have passive board wiring.
LEDC timer/channel 2 leaves display brightness and shutdown indication available.

Host profiles select 160×120/Q55, 320×240/Q75 (default), 640×480/Q75. Capture uses one YUYV422 PSRAM framebuffer. Preview returns it after copying the
immutable Core image, overlapping the next acquisition with app/UI processing.
WHEN_EMPTY parks DMA once that single queued frame is ready.
The app closes the camera before writing photos or buffered video to Flash. The existing Espressif JPEG encoder runs in the shared
vision worker with the sensor's YUYV input, avoiding an RGB conversion during
JPEG encoding. The pinned esp32-camera 2.1.8 dependency uses its Apache-2.0
and file-specific permissive licenses. Its public esp_jpeg dependency is
Apache-2.0. esp_new_jpeg retains its Espressif product-use license.
Two whole-row internal SRAM DMA nodes feed the PSRAM framebuffers. The ring
uses 3840 bytes for smaller profiles and 2560 bytes for VGA, avoiding corrupted
rows observed with direct PSRAM capture on this board. A board-local wrapper
overrides DMA geometry in the pinned camera driver and delegates other formats
to its original implementation. `PXA_PAI_CAMERA_DIAGNOSTIC` defaults to 0 and
must remain 0 in production builds.
Native preview converts full-range BT.601 YUYV into a bounded RGB565 Core IMAGE,
bypassing JPEG encoding/decoding. Video preview reuses the last JPEG capture.
The camera app is `local/pxa-apps/camera`, with automatic preview, profile
selection, JPEG photos and a thumbnail album for photos and MJPEG/Opus MKV videos.

## LCD DMA recovery

The board SPI adapter distinguishes a completed/dequeued DMA error from an
uncompleted transaction. It retires completed errors for LCD accounting, then
retries the entire frame after draining all DMA. After two underruns it uses
two reserved 1920-byte internal SRAM slots, retained until each corresponding
transaction is dequeued. The LCD-only chunk cap accounts for IDF rounding the
bus limit to a DMA descriptor. Other SPI devices retain their original path.
Successful normal frames continue to use direct PSRAM DMA.

The build-time `PXA_PAI_DISPLAY_DMA_TEST_FAULTS` defaults to 0. Nonzero test
builds inject that many completed DMA errors after GC2145 opens. Production
firmware must leave it at 0. The production camera app is
`local/pxa-apps/camera`, with JPEG photos and sequential MJPEG AVI playback.
