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
microphone capture uses the ES8389 ADC and is opened on demand through the
standard Audio service (16 kHz mono Opus). The RX DMA and encoder are separate
from playback; PCM reads run on the capture worker. Audio diagnostics report actual
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
An optional Core1 PIE straight-ARGB8888-to-RGB565 renderer is available with
`-DPXA_KORVO_LVGL_PIE=ON`. It keeps two SW workers on separate cores and uses
the native renderer on Core0 and for unsupported formats. It is off by default;
performance gains depend on how much of the UI uses matching straight-ARGB draws.
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

The image is flashed to the `pxa_data` partition at `0x700000`. The Korvo-1
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

## SD card and firmware layout

The microSD slot uses SDMMC slot 0, 4-bit at 20 MHz: D0..D3=GPIO20..23,
CLK=GPIO24, CMD=GPIO25 and active-low SD enable=GPIO39. The mappings and power
polarity follow the [official BSP](https://github.com/espressif/esp-bsp/tree/67f41a169401d1f23e827188dd08fb351cb0febe/bsp/esp32_s31_korvo_1).
A FAT card present at boot mounts at `/pxa/sdcard`, exposed as `/sdcard` in
Files/PXADB and as a removable volume through the standard Storage/FS services.
UTF-8 long names are enabled. Missing or invalid media never triggers formatting
and does not prevent startup. Insert/remove cards with the device powered off;
automatic hotplug mount/unmount is not implemented.

The application partition now ends at `0x700000` (size `0x6F0000`); `pxa_data`
occupies the remaining 9 MiB. The former 4 MiB application partition cannot hold
the expanded system. App-only upgrades are valid only with the matching new
partition table. Existing PXA installations using `pxa_data=0x410000` need an
explicit backup/migration before full flashing; never flash the larger app over
that old data partition. For a non-PXA installation, initial full flashing
replaces its original partitions.

This board has no battery gauge or IMU. Camera and peripheral features use the
negotiated workspace pxa-ext service; do not claim them as standard Sensor data.

## Camera

The OV3660 uses the IDF CAM/DVP controller: D0..D7=GPIO46..53, PCLK=54,
XCLK=55, VSYNC=56 and HREF=57. SCCB shares board I²C0 (SDA0/SCL1).
The CAM peripheral supplies 20 MHz XCLK before detection; the sensor driver
does not configure a second LEDC clock. Initialization uses the Apache-2.0
`esp_cam_sensor` 2.6.0 / `esp_sccb_intf` 1.0.0 components directly.

`org.doit.peripherals` 0.3 exposes the read-only `camera.jpeg` port. Request its
exact permission scope, open STREAM mode with zero parameter/address, and use
`<pxa-ext/vision.hpp>` to consume JPEG frames (320×240). A worker owns setup,
capture and teardown; aligned double DMA buffers preserve the first complete
image while stopping the controller. The driver waits at most three seconds
per capture. Shared budget allocation failures, sensor detection failures and
capture deadlines are reported to the application. Closing/backgrounding or
revoking permission releases the physical session after in-flight work ends.

See `extensions/peripherals/examples/camera` for permission, frame reading and
atomic private-file saving. Firmware does not assume every attached camera or
board revision is functional, and never programs eFuses to enable capture.

## RGB indicator

The on-board WS2812 (GPIO37) is enumerated as `indicator.rgb` by this workspace's
`org.doit.peripherals` / `pxa-ext` SDK. Negotiate its UUID/version, acquire the
exact port scope, open in RGB mode, then call `Port::rgb(r,g,b)` with values
0–255. A worker applies the latest accepted color, coalescing intermediate
writes. Exit, backgrounding and permission revocation queue black. Cleanup
failures retain the reservation and retry before another app can own the LED.

See `extensions/peripherals/examples/indicator`. This is an optional vendor
extension, separate from standard services; migrate to a standard interface
when the PXA protocol defines one.
