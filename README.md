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
local/pxa-apps/              Separate pxa-apps Git checkout (ignored by this workspace)
local/                       Ignored App catalog, overrides and outputs
deps/pxa-system/             Imported upstream system and package tooling
```

Clone the product App repository into `local/pxa-apps`; this workspace ignores
that checkout. The imported system also has App examples and signing fixtures
in `deps/pxa-system/apps/pxa`. The workspace App tooling detects the local
source root automatically, or accepts `--source-root` explicitly.

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

## Fast PXA Development

`tools/dev.sh` combines incremental App packaging, PXADB replacement and
launching into one development loop. An App source root is the parent directory
that contains `<app-id>/`; configure it once in ignored `local/apps.toml`, or
supply it for a one-off command:

```toml
# local/apps.toml
[apps.my-app]
source_root = "/home/me/work/pxa-apps"
```

Simulator mode keeps its PXADB2 service running and restarts the App after each
source save. Product-runner and Guest `pxa_log_*` output is streamed to the
terminal:

```sh
tools/dev.sh sim my-app --watch
# Or without a local catalog:
tools/dev.sh sim my-app --source-root /home/me/work/pxa-apps --watch
tools/dev.sh sim --board sensecap-watcher pixel-dungeon
```

Simulator mode selects the display profile matching `--board` by default;
pass `--profile` to override it.

Device mode independently builds an `esp32s3` package and replaces the App in
the existing firmware without reflashing it. It starts `pxadb logcat` after
each deployment and releases that USB Serial/JTAG connection before the next
update:

```sh
tools/dev.sh device my-app --port /dev/ttyACM0 --baud 2000000 --watch
```

Combined build, deployment and runtime output is also written to
`local/dev-logs/<mode>/<app-id>.log`. This is a fast WASM/AOT reload loop, not
runtime hot reload, so each source update restarts the App process.

## PXADB GUI

`tools/pxadb-gui.sh` opens a desktop front end for the same `pxadb` client used
by the CLI. It discovers USB devices and running product simulators, streams
device screenshots into a live preview, injects taps, drags and keys, browses
and transfers files, shows device logs and manages packages. The preview runs
at roughly 3-4 FPS on USB firmware using device-encoded JPEG and 15-20 FPS on
the product simulator; drags pause preview captures so input stays responsive.

```sh
tools/pxadb-gui.sh
tools/pxadb-gui.sh --port /dev/ttyACM0
tools/pxadb-gui.sh --simulator pai-touch
```

Because PXADB owns the serial port exclusively, close the GUI before using
`pxadb` or `tools/dev.sh device` on the same port. See
[PXADB GUI](tools/pxadb-gui/README.md).

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
tools/app.sh build pixel-dungeon --target simulator --source-root local/pxa-apps
tools/simulator.sh product --profile pai-touch \
  --package local/app-output/pai-touch/pxa-pixel-dungeon \
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
