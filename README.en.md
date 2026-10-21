# PXA ESP Platform

[简体中文](README.md)

An ESP-IDF product workspace that imports `pxa-system` without depending on a
legacy product firmware tree. It supports two board entry paths through the
same `pxa_board_port_t` contract:

- A new BSP implements the port directly in `firmware/boards/<board>/`.
- An existing firmware adds a small adapter that maps its current board API to
  the port after its display becomes ready.

## Developing an app? Start with DevKit

You do not need this workspace or ESP-IDF. Download [DevKit 0.4.0](https://pxa.doit.am/docs/downloads/), extract it, then run:

```sh
export PATH="$PWD/pxa-devkit-0.4.0-linux-x86_64/bin:$PATH"
pxa init hello
pxa run hello
```

C++26 and O3 are the defaults; `pxa.lock` pins the SDK and toolchain. The published binary requires Linux x86_64 and glibc 2.39+. Apps require Core 2.0 and their declared services.
[Getting started (Chinese)](https://pxa.doit.am/docs/first-app/) · [C++ SDK](deps/pxa-system/sdk/guest-cpp/README.zh-CN.md) · [Release](https://github.com/SmartArduino/pxa-system/releases/tag/v0.4.0) · [0.4.0 validation (Chinese)](docs/release-0.4.0.zh-CN.md)

The optional [PXA AI kit](https://pxa.doit.am/docs/ai-development/) adds development/debug/device/release skills, project-scoped agents and repeatable verification scripts. It works with the existing DevKit and adds no device runtime overhead.

Windows/macOS users can use [the cross-platform companion](tools/portable/README.en.md): build and run headless in a container, then use host-native PXADB for USB serial without forwarding devices.

The remaining commands are for system and board development, from this workspace root:

```sh
git clone --recurse-submodules https://github.com/SmartArduino/pxa-workspace.git
cd pxa-workspace
git clone https://github.com/SmartArduino/pxa-apps.git local/pxa-apps
```

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
that checkout. The system keeps legacy examples and signing fixtures
in `deps/pxa-system/apps/pxa`; new projects use the C++26 pxa init template. The workspace App tooling detects the local
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
build directory per board.

The `esp-mosaico` profile targets ESP32-S31 with a 480 × 480 CO5300 panel,
CST92xx touch, ES8311 playback and native USB CDC. See
[its board guide](firmware/boards/esp-mosaico/README.md) for revision detection,
the ESP-IDF main environment and hardware limitations.

`doit-gamekit` (doit游戏机 / DOIT GameKit) targets ESP32-S31 with a 320 × 240
ST7789, twelve gamepad buttons, MAX98357A audio and an SPI SD card, without
touch. It uses button focus navigation without a bottom navigation bar, plus
a fallback mouse pointer. Landscape frames are rotated in software and sent in
ST7789 native scan order, without a TE signal. Build with
`tools/firmware.sh doit-gamekit build`; see the
[board guide](firmware/boards/doit-gamekit/README.md) for wiring and controls.

`doit-gamekit-1` targets the V0.8 revision with ST7789V TN hardware landscape
initialization from the reference firmware, FT6336U touch, 14 keys including
MENU/RETURN, MAX98357A audio and native four-bit SDMMC. Build with
`tools/firmware.sh doit-gamekit-1 build`; see the
[board guide](firmware/boards/doit-gamekit-1/README.md).

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

The command argument names the source directory. The output file uses
`pxa-<app-id>.pxa`, while installation and launch use the ID in the signed
package manifest. These names may differ.

```toml
# local/apps.toml
[apps.my-app]
source_root = "/home/me/work/pxa-apps"
```

Simulator mode installs the App, starts the full PXA system (the same path as
`tools/simulator.sh run`) and automatically opens the App. The launcher,
settings, system input method, theme, DPI and display shape are available.
It keeps its PXADB2 service running and restarts the system with the updated App
after each source save. Host and Guest `pxa_log_*` output is streamed to the terminal:

```sh
tools/dev.sh sim my-app --watch
# Or without a local catalog:
tools/dev.sh sim my-app --source-root /home/me/work/pxa-apps --watch
tools/dev.sh sim --board sensecap-watcher pixel-dungeon
tools/dev.sh sim --board esp-mosaico voxel-craft
```

Simulator mode selects the display profile matching `--board` by default;
pass `--profile` to override it. Settings and App data use the same persistent
profile state as `tools/simulator.sh run`. Use `--instance <name>` to keep a
separate development instance. For standalone App testing, use
`tools/simulator.sh product --profile <board> --installed <package-id>`.
`dev.sh` packages only the current target's AOT, omitting the `.wasm` fallback
(including components declaring `artifact: "both"`);
explicitly declared Wasm-only components are preserved. Use `tools/app.sh build`
directly when you need both artifacts.

Device mode reads `PXA_BOARD_TARGET` from `firmware/boards/<board>/board.cmake`
(default: pai-touch / esp32s3). New boards using a supported architecture need
no tool-side board-name mapping. `tools/app.sh` and the package wrapper use the
same reader; explicit `--target` overrides the default, and missing declarations
fail with an error. Device mode replaces the App in
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

Standard UI mode runs the complete system and launches installed desktop Guest apps. Manifest-only launcher cards still require a built, installed package.

```sh
tools/dev.sh sim pixel-dungeon --source-root local/pxa-apps
# Run an installed app directly, without the system launcher:
tools/simulator.sh product --profile pai-touch --installed pxa-pixel-dungeon
```

The simulator uses the same signature and artifact-selection contracts as devices. It validates layout, input and Guest behavior. Drivers, DMA, touch ICs, audio output latency and power must also be checked on hardware. See [Simulator](simulator/README.md).

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
