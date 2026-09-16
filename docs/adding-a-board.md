# Adding A Board

[简体中文](zh-CN/adding-a-board.md)

Create `firmware/boards/<id>/` as an ESP-IDF component with a `CMakeLists.txt`,
`idf_component.yml`, `src/` and `include/`. Implement and register one
`pxa_board_port_t` instance.

Add `board.cmake` with `set(PXA_BOARD_TARGET "...")` and
`set(PXA_BOARD_COMPONENT "...")`. `PXA_BOARD_COMPONENT` is the ESP-IDF
component that exports `pxa_board.h`; it is usually the board directory name.
The top-level project reads this metadata before ESP-IDF component discovery,
so the selected board, toolchain and dependency solver always use the same
target.

Put packages common to every board in `firmware/main/idf_component.yml`. Put display,
touch, codec, cellular, power and other physical-board packages only in
`firmware/boards/<id>/idf_component.yml`. ESP-IDF processes manifests for the selected
component, so an unselected board does not enter the dependency graph. Keep
the generated `dependencies.lock.<board>` in version control; never edit it
manually.

Required callbacks are `initialize`, `display` and `display_profile`. The
system can start on a display-only board. Add `set_level`,
`set_network_enabled`, `system_ready` and diagnostics only when the hardware
supports them.

Keep high-rate rendering in the board. For example, a board-specific presenter
may acquire a PXA surface and submit it to SPI DMA directly. Do not copy frames
through `pxa_board_port_t` or route individual pixels, audio samples or DMA
chunks through generic callbacks.

For an existing product, the board port may be a `legacy_board_adapter.cc`
alongside the existing board source. It wraps public legacy APIs and does not
require relocating the original driver or business code.

Each board exports `include/pxa_board.h` with `pxa_board_register_selected()`.
The product `main` calls only this stable function, so selecting a board via
`-DPXA_BOARD=<id>` does not require changing the application entry point.

Define a data/littlefs entry in the board's `partitions.csv`, then set its
matching `CONFIG_PXA_STORAGE_PARTITION_LABEL` and an absolute
`CONFIG_PXA_MOUNT_POINT` in the board `sdkconfig.defaults`. These are board
properties, not fixed `assets` names. Select a product-owned
`CONFIG_PXA_BUILTIN_PACKAGE_ROOT` for optional factory Packages. Firmware
builds never create or flash this partition; see
[PXA App Delivery](pxa-app-delivery.md) for pxadb development delivery and
factory image generation. Add a matching `simulator/profiles/<id>.toml` only
for display and input semantics; it must not copy board drivers.
