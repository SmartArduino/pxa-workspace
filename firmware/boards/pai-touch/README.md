# pai-touch Board

[简体中文](README.zh-CN.md)

This directory is one ESP-IDF component, selected with `PXA_BOARD=pai-touch`.
It contains the sample hardware implementation: JD9853 display setup, CST826
touch, ADC buttons, battery monitor, Wi-Fi and RPC701 audio.

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
