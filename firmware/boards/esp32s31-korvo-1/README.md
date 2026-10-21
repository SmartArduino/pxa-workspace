# ESP32-S31-Korvo-1

This ESP-IDF 6.2 board profile targets the RISC-V `esp32s31`. It configures
the Korvo-1 800x480 RGB565 triple framebuffer, GT1151 touch over I2C0 and
the ES8389 speaker codec over I2S0. It enables VSYNC-synchronized direct
triple-buffer scanout, two LVGL software renderers, Wi-Fi and volume controls.
The measured active display area is 9.5 x 5.5 cm. Its diagonal pixel density
is approximately 216 DPI; both the board and simulator publish this density
to the system UI and PXA apps (1.35 pixels per 160-DPI reference unit).
Display setup holds the LVGL port lock until a dark, built-in-font
`PXA / Starting...` screen is installed. The first frame is presented before
touch, audio, Wi-Fi and storage initialization. System UI creation removes
the boot content and explicitly refreshes the display. Early console markers
`Boot frame visible` and `Startup ... ready` report milliseconds since boot.
This panel has no backlight control; software cannot hide panel behavior
before LCD initialization.
The DAC uses three 320-frame I2S DMA blocks at 16 kHz, paced by the device;
the unused microphone channel is not allocated. Audio diagnostics report actual
nonzero DMA completions and the first PCM-to-DMA-block completion interval.
That interval excludes Guest dispatch and is not an acoustic latency measurement.
Performance builds use O3 for the portable rasterizer. The measured 314-byte
depth/texture span kernel is placed in IRAM through an IDF linker fragment;
other boards default to mapped code. Set `-DPXA_RASTER_SPAN_IRAM=OFF` in
`idf.py` to compare placement.
GT1151 contacts are exposed as five independent LVGL pointer devices, so PXA
apps and games receive stable `pointer_id` values for simultaneous touches.
The poller checks the GT1x data-ready bit before using the GT1151 driver for
report reads, checksums and acknowledgements. An empty, unready mailbox keeps
the previous contacts and is never acknowledged; a ready zero-contact report
releases immediately. Only communication errors use the bounded 150 ms timeout,
measured from the last healthy read, including polls without a new report.
Touch I2C uses the official board profile's 400 kHz rate and a 20 ms transaction
timeout. Pointer callbacks share a snapshot; the next poll window starts after
the transfer finishes so a slow read does not immediately cause another read.
Controller report reordering keeps existing pointer IDs stable; duplicate IDs
are ignored. Out-of-bounds coordinates preserve a known contact's last valid
position instead of generating a false release. The state uses fixed storage.
Run `bash firmware/boards/esp32s31-korvo-1/tests/test_host.sh` for touch-state
ASan/UBSan regression checks. To capture physical touch edges, build with
`-DPXA_KORVO_TOUCH_TRACE=ON`; it records report counts, idle polls, errors, read
timing and physical pointer edges. It is off by default and excludes injected
input.
Build it with:

```sh
tools/firmware.sh esp32s31-korvo-1 build
```

Use `/dev/ttyUSB0` to flash or monitor the connected board.
PXADB uses 2000000 baud. Check the generated console configuration when
diagnosing early boot: ESP-IDF's default UART choice forces 115200 baud even
if the defaults request 2000000; PXADB switches the UART after initialization.
To update firmware while preserving installed
apps, fonts and data:

```sh
tools/firmware.sh esp32s31-korvo-1 app-flash --port /dev/ttyUSB0 --baud 2000000
```

Create the factory `pxa_data` image with the CJK system font included:

```sh
tools/pxa/build_partition_image.sh --include-system-font \
  --output out/pxa-partitions/esp32s31-korvo-1/pxa_data-fonts.bin \
  esp32s31-korvo-1
```

The image is flashed to the `pxa_data` partition at `0x410000`. The Korvo-1
has four calibrated ADC keys on GPIO42: MODE returns to the launcher, SET
starts Wi-Fi provisioning, and VOL-/VOL+ control the active app or system
volume. The LCD subboard has no routed backlight PWM, so brightness is
intentionally reported as unsupported.

For on-device Wi-Fi setup, open **Settings → Wi-Fi**, select a scanned
network, and enter its password using the on-screen keyboard. Use **Scan again**
to refresh the list; the SET key remains an alternative provisioning method.
After Wi-Fi obtains an IP address, the board synchronizes its system clock
with `pool.ntp.org`. A later reconnection starts synchronization again; the
ESP-IDF SNTP service also refreshes the clock periodically while connected.
