# Workspace Layout

[简体中文](zh-CN/workspace-layout.md)

The workspace has four independent development paths:

| Path | Owns | Does not own |
| --- | --- | --- |
| `firmware/` | ESP-IDF product, boards, drivers and pxadb service | PXA App source and factory image creation |
| `factory/` | Versioned per-board factory selection and static base content | Developer-specific source paths |
| `simulator/` | Desktop display and input profiles | ESP-IDF drivers, DMA and physical peripherals |
| `local/` | Private App catalog, temporary factory overlays and output | Any tracked product policy |

## Board Work

Place a board in `firmware/boards/<id>/`. Its component owns drivers,
`idf_component.yml`, `sdkconfig.defaults`, `partitions.csv` and the
`pxa_board_port_t` implementation. Use the wrapper so every board gets a
separate build directory:

```sh
tools/firmware.sh pai-touch build
tools/firmware.sh pai-touch flash --port /dev/ttyACM0 --baud 406800
tools/firmware.sh pai-touch monitor --port /dev/ttyACM0 --baud 115200
```

Do not put board-specific drivers in generic PXA components. Existing products
can keep their original structure and add a local adapter beside the board code.

## App Work

PXA App sources normally live outside this workspace, often in separate App
repositories. Map an App ID to the parent source root in ignored
`local/apps.toml`, then package it without involving ESP-IDF:

```sh
tools/app.sh build pai-touch-diagnostics --board pai-touch
```

The generated `.pxa` is the artifact for pxadb installation. It is not a
firmware dependency and is never included by a normal firmware build.

## Factory Work

`factory/profiles/<board>.toml` is the reproducible, tracked list of Apps and
base content for one board. Source locations remain local. Temporary or private
Apps belong in an ignored overlay passed to `tools/factory.sh image`. A factory
image replaces the full PXA storage partition; use pxadb for normal App cycles.

## Simulator Work

Use `tools/simulator.sh` for standard UI layout, mouse/touch input and
display-profile validation. It uses `generic` by default; select a workspace
profile with `--profile`. It runs the imported desktop simulator with an
optional external App manifest catalog:

```sh
tools/simulator.sh --profile pai-touch --app-root /work/pxa-apps
```

This is deliberately not a hardware emulator. Verify display-controller setup,
DMA, touch IC behavior, audio and power on firmware and the selected board.
For real Guest execution, build an independent desktop package with
`tools/app.sh build <app-id> --target simulator`. Run its unpacked signed
directory through `tools/simulator.sh product --package <directory>`, or use
the persistent local PXADB loop:

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-<app-id>.pxa --simulator pai-touch
tools/simulator.sh product --profile pai-touch --installed pxa-<app-id>
```

Installed state lives in ignored `local/simulator/<profile>/`; see
[Simulator](../simulator/README.md).
