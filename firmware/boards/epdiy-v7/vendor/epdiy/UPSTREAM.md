# Driver provenance

Vendored from the user-supplied reference:

`/home/lucinhu/Documents/workspace/esp32/epdiy_v7_40pin_factory_waveform_normalized/components/epdiy`

Reference component metadata identifies EPDiy 2.0.0, licensed
[LGPL-3.0-or-later](LICENSE), upstream [vroland/epdiy](https://github.com/vroland/epdiy).
The reference contains local changes, so this snapshot is not represented as an unmodified upstream
release. Keep this source available with firmware distributions under the applicable license.

Only the ESP32-S3 LCD output, common renderer, font support, V7 board/power drivers and ED047TC2
waveform are included. Demo assets, other boards and unrelated waveforms are excluded. `panel.c`
names the custom 1216×684 / 16-bit / bus_speed=24 panel explicitly.

Local changes relative to that reference:

- Drop the unused ESP32 I2S output header from the S3-only renderer.
- Use `CONFIG_SPIRAM` for the IDF 5.5 PSRAM check.
- Return immediately on draw failure before committing `back_fb`.
- Commit only the actual changed rectangle; LCD output still receives the full scan dimensions
  and dirty masks, preserving the verified DMA prefill and source alignment.
- Use the correct pixel parity when finding the rightmost changed column.
- Select the interval whose lower bound contains the temperature, clamping at either end.
- Point the seven retained waveform ranges (`_5` through `_11`) at original intervals 5–11
  (15–38°C). The waveform LUT bytes and phase timing values are unchanged.
- Allocate the line queue's pointer table by pointer count, rather than by row data size.
- Reset software line queues between LCD phases after both feeders and DMA finish; cap prefill
  at one queue's capacity so padded tail rows cannot block the next phase before DMA starts.

The board presenter uses the existing S3 vector path with `EPD_LUT_1K`; this path uses the same
1KB conversion tables even if EPDiy is initialized with `EPD_LUT_64K`.
