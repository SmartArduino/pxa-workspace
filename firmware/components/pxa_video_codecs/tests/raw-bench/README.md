# ESP-Mosaico raw H.264 benchmark

This standalone ESP-IDF project runs the Espressif decoder on two Annex B
streams linked into the application image. It measures zero-copy YUV420,
packed RGB565 from the decoder, and RGB565 converted from YUV with the PPA.
It excludes the LCD transfer, audio, Wi-Fi, and PXA runtime. Timings include
the decoder drain and exclude USB reporting. The final line reports the
number of decoded pictures, elapsed time, and the lowest free heap during
each run.

Run the fixture generator with Python and FFmpeg's `ffprobe`. It chooses the
previous key packet if `--start-frame` lands inside a group of pictures.
Set `run_dir` to the run's directory under the repository's ignored
`local/artifacts/`.

```sh
python prepare_clip.py input-360.h264 "$run_dir/clip-360.bin" --frames 250
python prepare_clip.py input-480.h264 "$run_dir/clip-480.bin" --frames 250
```

Use ESP-IDF with ESP32-S31 support and the `esp_openh264` package reviewed by
the firmware's binary audit. Supply local copies of the board's locked
`esp_tinyusb` and `tinyusb` components under those directory names. The
benchmark uses the existing Mosaico partition layout.

```sh
idf.py -B "$run_dir/build" -DSDKCONFIG="$run_dir/sdkconfig" \
  -DOPENH264_DIR="$openh264_dir" -DTINYUSB_COMPONENTS="$usb_components" \
  -DCLIP_0="$run_dir/clip-360.bin" -DCLIP_1="$run_dir/clip-480.bin" build
```

The default build uses the Minimal decoder profile and executes from NOR.
To test Performance with PSRAM XIP, use an empty build directory and a fresh
`SDKCONFIG` path with
`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.performance.defaults"`.

After saving the board's current application image, flash only the benchmark
application at `0x10000` using the repository's `tools/mosaico_esptool.py`
wrapper. Run `python run_bench.py --port "$port" --output "$log"` once the
Mosaico USB CDC port appears. The same wrapper can restore the original
application after the run.
