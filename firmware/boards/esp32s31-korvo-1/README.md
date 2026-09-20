# ESP32-S31-Korvo-1

This ESP-IDF 6.2 board profile targets the RISC-V `esp32s31`. It configures
the Korvo-1 800x480 RGB565 double framebuffer, GT1151 touch over I2C0 and
the ES8389 speaker codec over I2S0. It enables VSYNC-synchronized direct
double-buffer scanout, two LVGL software renderers, Wi-Fi and volume controls.
GT1151 contacts are exposed as five independent LVGL pointer devices, so PXA
apps and games receive stable `pointer_id` values for simultaneous touches.
Build it with:

```sh
tools/firmware.sh esp32s31-korvo-1 build
```

Use `/dev/ttyUSB0` to flash or monitor the connected board.

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
