# PXA ESP Platform

[简体中文](README.zh-CN.md)

An ESP-IDF product workspace that imports `pxa-system` without depending on a
legacy product firmware tree. It supports two board entry paths through the
same `pxa_board_port_t` contract:

- A new BSP implements the port directly in `firmware/boards/<board>/`.
- An existing firmware adds a small adapter that maps its current board API to
  the port after its display becomes ready.

## Layout

```text
firmware/                    The only ESP-IDF project
firmware/boards/             Per-board drivers, defaults and partitions
simulator/                   Desktop UI/display-profile simulation profiles
factory/                     Reproducible board-specific factory profiles
tools/                       Firmware, App, simulator and factory entry points
local/                       Ignored local App catalog, overrides and outputs
deps/pxa-system/             Imported upstream system and package tooling
```

`firmware/boards/pai-touch` is itself an ESP-IDF component. Select another board with
the CMake cache value `PXA_BOARD`; no global board-type Kconfig choice is
required. Its `sdkconfig.defaults` is loaded as a board overlay after the
cross-board root defaults.

## Managed dependencies

ESP-IDF dependencies are declared beside the code that owns them. The product
baseline is in `firmware/main/idf_component.yml`; every board declares its display,
input, power, connectivity and optional font packages in
`firmware/boards/<board>/idf_component.yml`. This keeps boards with incompatible driver
versions in separate dependency graphs. The Component Manager generates the
board-specific `dependencies.lock.<board>` file; commit it after dependency
resolution, but do not edit it manually.

## Build a selected board

Build and debug board code through the board wrapper. It uses an independent
build directory per board:

```sh
tools/firmware.sh pai-touch build
tools/firmware.sh pai-touch flash --port /dev/ttyACM0 --baud 406800
tools/firmware.sh pai-touch monitor --port /dev/ttyACM0 --baud 115200
```

The equivalent direct ESP-IDF build command is:

```sh
idf.py -C firmware -B build/firmware/pai-touch -DPXA_BOARD=pai-touch build
```

`PXA_BOARD` selects `firmware/boards/<board>/`, its `board.cmake`,
`sdkconfig.defaults`, partition table and `idf_component.yml`. `board.cmake`
selects the matching ESP target before component discovery. The first
configuration resolves the selected board's managed components and generates
`firmware/dependencies.lock.<board>`. Do not reuse a build directory for
another board.

## PXA App Delivery

Firmware builds and PXA App delivery are intentionally separate. App source
roots are resolved from ignored `local/apps.toml`, so private Apps do not dirty
this workspace. Normal firmware builds and flashes neither package Apps nor
write the PXA storage partition. See
[PXA App Delivery](docs/pxa-app-delivery.md).
Directory ownership and daily workflows are documented in
[Workspace Layout](docs/workspace-layout.md).

## Simulator

`tools/simulator.sh` runs the imported SDL/LVGL standard UI simulator with the
default `generic` profile. Select another workspace profile explicitly:

```sh
tools/simulator.sh --profile pai-touch
```

Supply an external App manifest root with `--app-root <root>`. This UI mode is
intentionally limited to UI, input and display-profile work; its launcher cards
do not execute Guest bytecode. For package-level App work, build a desktop
artifact and run its signed package directory with `product` mode:

```sh
tools/app.sh build arcade --target simulator --source-root deps/pxa-system/apps/pxa
tools/simulator.sh product --profile pai-touch \
  --package local/app-output/pai-touch/pxa-arcade \
  --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der
```

Product mode verifies the signature, selects `linux-x86_64` WAMR AOT, and runs
the Guest through the window, UI, clock, audio and permission host services.
For persistent desktop installation, start `tools/simulator.sh service start
--profile pai-touch`, install with `pxadb package install <app>.pxa --simulator
pai-touch`, then launch with `tools/simulator.sh product --installed <app-id>`.
See [Simulator](simulator/README.md) for details.

## Board Contract

The board port provides display creation/profile data and optional controls for
network, volume, brightness, status publication and diagnostics. It is control
plane only. DMA submit, framebuffer ownership, pixel conversion and PCM frame
transfer stay in the board component. The pai-touch direct scanout path is an
example of this separation.

## Existing Firmware

Do not move the old board implementation. Add an adapter adjacent to it that
registers a `pxa_board_port_t`, then call `pxa_integration_start()` after the
existing LVGL display has been initialized. The adapter owns all references to
legacy `Board`, UI or product classes; neither `pxa-system` nor the generic
integration component includes them.

## Dependencies

`deps/pxa-system` is a standalone Git checkout pinned with its WAMR and
page-manager submodules. The ESP host owns its small WAMR/ESP-IDF declaration
shim directly. Source and CMake files contain
no path to the original pai-touch demonstration firmware.
