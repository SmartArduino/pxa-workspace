# ESP-Mosaico native online H.264 benchmark

This standalone ESP-IDF application plays the 360p AVC/AAC DASH streams for
Bilibili video `BV1oEy5zaE8y`. It requests fresh media URLs from Bilibili and
keeps at most two downloaded ranges per track in PSRAM. Downloads resume
from the last validated byte if a range response stops early. The LCD task sends
packed RGB565 from the decoder to the CO5300 over QSPI DMA. The audio task
decodes AAC and feeds I2S to the board's ES8311 DAC. It uses Billy's DASH and
MP4 parsers without linking the PXA runtime.

Use the ESP32-S31 ESP-IDF toolchain and the same licensed component versions
as the board build. In the ignored run directory, put local copies or
symlinks to `esp_tinyusb`, `tinyusb`, `esp_audio_codec`, `esp_codec_dev`,
`esp_lcd_co5300`, and `cmake_utilities` under `components/`. Set
`OPENH264_DIR` to the local Espressif library.

```sh
idf.py -B "$run_dir/build" -DSDKCONFIG="$run_dir/sdkconfig" \
  -DBOARD_COMPONENTS="$run_dir/components" -DOPENH264_DIR="$openh264_dir" build
```

For the Performance profile with PSRAM XIP, use a fresh build directory and
SDKCONFIG path and add
`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.performance.defaults"`.
Both builds select `FAST` at runtime. The Performance build uses software
AES for TLS and PSRAM for I2S DMA to fit the smaller internal memory budget.
Set `BILIBILI_DIR` if the Billy checkout
is outside the workspace's `local/pxa-apps/bilibili`.

Save the original application. Use the repository's
`tools/mosaico_esptool.py` wrapper to flash the benchmark application image
at `0x10000`. Start playback and recording with the serial runner.

```sh
python run.py --port "$port" --ssid "$ssid" --output "$run_dir/serial.log"
```

The runner prompts for the Wi-Fi password and sends it over USB CDC with echo
disabled. The firmware uses RAM-only Wi-Fi settings and plays one time per
boot. Restore the saved application at the same offset after testing.

The serial log includes completed LCD transfers, H.264 call time, PCM
packets accepted by I2S, download time, and time consumers waited for ranges.
Per-window display stats count completed transfers over wall time.
`pcm_gaps` counts inter-write gaps longer than the I2S DMA ring can cover.
