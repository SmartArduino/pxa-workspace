# PXA App Delivery

[简体中文](zh-CN/pxa-app-delivery.md)

Firmware and PXA Apps have separate delivery lifecycles in this workspace.
An ESP-IDF build compiles only firmware. It does not build a PXA App, generate
a LittleFS image, or add the PXA storage partition to `idf.py flash`. A normal
firmware flash therefore leaves installed Apps and user data intact.

The selected board configures PXA storage in both places below. The partition
label must match exactly.

```text
firmware/boards/<board>/partitions.csv
  <label>, data, littlefs, ...

firmware/boards/<board>/sdkconfig.defaults
  CONFIG_PXA_STORAGE_PARTITION_LABEL="<label>"
  CONFIG_PXA_MOUNT_POINT="/<mount>"
  CONFIG_PXA_BUILTIN_PACKAGE_ROOT="system/pxa/factory"
```

`pai-touch` uses the `pxa_data` label and mounts it at `/pxa`. The name is a
board decision; `assets` is only the generic compatibility default.

## Development delivery through pxadb

Register the App's external source root in ignored `local/apps.toml`, then
package it independently of firmware:

```sh
tools/app.sh build <app-id> --board pai-touch
```

The command produces:

```text
local/app-output/pai-touch/pxa-<app-id>/
local/app-output/pai-touch/pxa-<app-id>.pxa
local/app-output/pai-touch/pxa-<app-id>.pxa.provenance.json
```

Use the `.pxa` container with pxadb. The client uploads it to
`<CONFIG_PXA_STATE_ROOT>/inbox/`, then invokes the PXA package `install`
action for the App identity. The firmware validates and commits it into its
managed Package directory. Rebuilding or flashing only firmware later does not
reinstall or delete that App.

Install the workspace-local host client once, then deploy an App to the device:

```sh
python3 -m pip install -e tools/pxadb
pxadb package install local/app-output/pai-touch/pxa-<app-id>.pxa \
  --port /dev/ttyACM0
```

`pxadb package install` performs both staging and installation. Use `--yes` for
a non-interactive replacement of an installed App. `pxadb package list` shows
the resulting package state. The host client uses the selected board's PXADB
USB Serial/JTAG service; it does not depend on another firmware repository.

Use `--source-root <root>` instead of the local catalog for a one-off build.
`PXA_SIGNING_KEY` selects the signing key. No App source or output needs to be
inside this repository, and the command does not invoke ESP-IDF.

## Factory image

Factory profiles are versioned separately from App source paths. Build a full
storage-partition image explicitly:

```sh
tools/factory.sh image pai-touch
```

By default the image is written to:

```text
out/pxa-partitions/pai-touch/pxa_data-empty.bin
```

Add release App IDs to `factory/profiles/pai-touch.toml`; their source roots
are resolved from `local/apps.toml`. Use an ignored overlay for private or
temporary Apps:

```sh
tools/factory.sh image pai-touch --overlay local/factory-pai-touch.toml
```

Use `--output <image>` to choose another path. The tool reads the selected
board's partition label, offset, size, LittleFS name limit and factory Package
root, then prints the exact offset with the image path. Factory Apps are placed
under `CONFIG_PXA_BUILTIN_PACKAGE_ROOT`; the tracked pai-touch profile includes
the product font by default.

Flashing this image is a factory-provisioning operation because it replaces the
entire PXA storage partition. Do not use it for routine firmware updates or App
development on a device that already has installed Apps. Use pxadb instead.

If no factory image is provisioned, the default
`CONFIG_PXA_STORAGE_FORMAT_IF_MOUNT_FAILED=y` formats an uninitialized PXA
storage partition at first boot. It creates an empty installation area so
pxadb can install Apps. Disable that option for products that must preserve a
mount failure for service recovery rather than reformatting it.
