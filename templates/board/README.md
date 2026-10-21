# Board Component Template

[简体中文](README.zh-CN.md)

Copy this directory to `firmware/boards/<board-id>/`. The directory itself is the
ESP-IDF component selected by `PXA_BOARD`.

Keep the board component self-contained:

- `idf_component.yml` declares only physical-board dependencies.
- `board.cmake` declares the ESP target used by this board.
- `sdkconfig.defaults` and `partitions.csv` define board-specific build and
  flash layout. Define one `data/littlefs` PXA storage partition and set its
  exact label in `CONFIG_PXA_STORAGE_PARTITION_LABEL`, together with an
  absolute `CONFIG_PXA_MOUNT_POINT`.
- `include/pxa_board.h` exports `pxa_board_register_selected()`.
- `src/` contains drivers and the `pxa_board_port_t` implementation.

Do not add a board's dependencies to `firmware/main/idf_component.yml` unless every
supported board needs them.

[pxa_system_clock](../../firmware/components/pxa_system_clock/README.md) supplies
a ticking default time and restores the time saved at shutdown. Call
`pxa_system_clock_update_status()` for initial and periodic status, and
`pxa_system_clock_save()` before cutting power, disabling RTC buses or entering
deep sleep. Software restarts save automatically. Register UTC read/write
callbacks with `pxa_system_clock_set_source()` during board initialization to
use an external RTC, an on-chip RTC or a companion processor.

PXA storage is not a fixed `assets` partition and is not flashed as part of a
normal firmware update. See [PXA App Delivery](../../docs/pxa-app-delivery.md)
for pxadb development installation and factory image generation.
