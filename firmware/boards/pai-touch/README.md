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
  control. The link only drains them and ignores the contents, so receive
  overflows during the post-wake backlog are reported at most once every 10 s.

The two CST826 contacts are exposed as independent LVGL pointer devices, so PXA
apps receive stable `pointer_id` values for simultaneous touches.

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
