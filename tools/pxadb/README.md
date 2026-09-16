# PXADB

PXADB is the workspace-local host client for the firmware PXADB USB
Serial/JTAG service. It installs independently built PXA containers without
rebuilding or flashing firmware.

```sh
python3 -m pip install -e tools/pxadb

# Install one independently built PXA App.
pxadb package install local/app-output/pai-touch/pxa-example.pxa \
  --port /dev/ttyACM0

# Inspect the installed package catalog.
pxadb package list --port /dev/ttyACM0
```

The client stages a local `.pxa` into the firmware inbox, asks the firmware to
verify its signature and inventory, then commits it through the normal package
transaction. Passing `--yes` permits a non-interactive replacement. `--port`
is optional when exactly one PXADB USB device can be identified.

For the product simulator, start its local service and select a profile instead
of a serial port:

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-example.pxa --simulator pai-touch
pxadb package list --simulator pai-touch
pxadb package run pxa-example --simulator pai-touch
```

`pxadb devices` automatically lists every running local simulator profile,
alongside discoverable USB devices.
When exactly one verified PXADB endpoint is available, all PXADB commands use
it automatically; otherwise use `--simulator PROFILE` or `--port PATH`.

`--simulator` uses PXADB2 binary frames over a per-user Unix socket. To connect
to an explicitly enabled trusted-LAN listener, pass its token file:

```sh
pxadb package list --connect 192.168.1.20:9222 \
  --token local/simulator/pai-touch/pxadb2.token
```

TCP token authentication does not encrypt traffic. The UI-only desktop
simulator has no PXADB endpoint. See [PXADB2 Binary Transport](../../docs/pxadb2.md)
for the binary framing contract.

Stop or inspect one service with `tools/simulator.sh service stop --profile
pai-touch` and `tools/simulator.sh service status --profile pai-touch`.
Use `--instance` to run isolated copies of one profile; PXADB addresses that
copy as `PROFILE@INSTANCE`. `--listen HOST:auto` asks the OS to choose a free
TCP port and records the actual listener in `local/simulator/PROFILE@INSTANCE/pxadb2.tcp`.

## 简体中文

PXADB 是本工作区固件 PXADB USB Serial/JTAG 服务的主机客户端。它可安装独立构建的
PXA 容器，不会重新编译或烧录固件。

```sh
python3 -m pip install -e tools/pxadb

# 安装一个独立构建的 PXA App。
pxadb package install local/app-output/pai-touch/pxa-example.pxa \
  --port /dev/ttyACM0

# 查看已安装的 Package catalog。
pxadb package list --port /dev/ttyACM0
```

客户端将本地 `.pxa` 暂存到固件 inbox，请求固件校验签名和文件清单，再通过常规
Package 事务提交。传入 `--yes` 可进行非交互式替换。只有一个可识别的 PXADB USB 设备时，
可以省略 `--port`。

产品模拟器可启动本地服务，并用 Profile 代替串口：

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-example.pxa --simulator pai-touch
pxadb package list --simulator pai-touch
pxadb package run pxa-example --simulator pai-touch
```

`pxadb devices` 会自动列出每个正在运行的本地 simulator Profile，以及可发现的 USB 设备。
只有一个已验证的 PXADB 端点时，所有 PXADB 命令会自动使用它；否则请传入
`--simulator PROFILE` 或 `--port PATH`。

`--simulator` 通过每用户 Unix socket 使用 PXADB2 二进制 frame。连接显式开启的受信任局域网
listener 时，传入 token 文件：

```sh
pxadb package list --connect 192.168.1.20:9222 \
  --token local/simulator/pai-touch/pxadb2.token
```

TCP token 认证不加密流量。仅 UI 的桌面模拟器仍没有 PXADB 端点。
二进制 frame 协议见 [PXADB2 Binary Transport](../../docs/pxadb2.md)。

可用 `tools/simulator.sh service stop --profile pai-touch` 停止一个服务，或用
`tools/simulator.sh service status --profile pai-touch` 查看状态。`--instance`
可启动同一 Profile 的隔离副本，PXADB 地址写为 `PROFILE@INSTANCE`。`--listen HOST:auto`
会让系统选择空闲 TCP 端口，并把实际 listener 写入
`local/simulator/PROFILE@INSTANCE/pxadb2.tcp`。
