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

# Launch or stop an installed App without navigating the launcher.
pxadb package run pxa-example --port /dev/ttyACM0
pxadb package stop pxa-example --port /dev/ttyACM0
```

The client stages a local `.pxa` into the firmware inbox, asks the firmware to
verify its signature and inventory, then commits it through the normal package
transaction. Passing `--yes` permits a non-interactive replacement. `--port`
is optional when exactly one PXADB USB device can be identified.
The deployment ID comes from the signed package manifest, so a renamed `.pxa`
file still installs under the correct App ID. An explicit `--identity` must
match that manifest ID; a mismatch is rejected before upload.

New firmware advertises `package_receive=1` and `package_directory_adopt=1`
in `INFO`. CLI/GUI installation uploads into an invisible receiving candidate,
then verifies and deploys on the same connection. Directory packages are moved
into the recoverable installation transaction without copying every file again.
Successful and failed installs remove their staging source. Cancellation and
graceful disconnect remove partial uploads; abrupt disconnects are cleaned on
reconnection, the 30-second idle timeout, or reboot. Explicit `package stage`
retains a fully verified source until installed or removed. See
[receive lifetime and storage](../../docs/pxadb2.md#firmware-receive-lifetime-and-storage).

Serial uploads select a chunk size from the detected USB device: 256 bytes for
Espressif USB Serial/JTAG, or 512 bytes with paced writes for USB-to-UART
bridges. Unknown ports keep the conservative 64-byte default. Set
`PXADB_UART_UPLOAD_CHUNK` to override the chunk size if needed. UART boards
configured for a non-default baud rate also require `--baud`, for example
`--baud 2000000`.

Interrupted offset uploads resume at the device-confirmed position. A malformed
serial chunk restarts the digest-checked upload with smaller bursts, within the
same bounded retry limit; storage failures and package validation failures are
reported immediately. On a noisy high-speed UART, `PXADB_UART_UPLOAD_CHUNK=256`
can also select smaller bursts from the start.

Firmware advertising `fs_chunk_crc32=1` also accepts
`FSDATA2 OFFSET CRC32_HEX BASE64`. The client selects it automatically for
offset serial uploads. CRC32 uses the IEEE variant, encoded as eight hex
digits, and is checked before writing each decoded block. Rejected or
truncated blocks leave the confirmed prefix intact for a bounded retry.
The full-file SHA-256 still gates publication. Older firmware keeps `FSDATA`,
and the binary transport keeps its existing frame integrity checks.

Firmware with the `memory` capability provides an on-demand diagnostic
snapshot: `pxadb memory --port /dev/ttyACM0` (add `--baud 2000000` for Korvo).
The JSON separates ESP heap-capability totals/free/minimum free from PXA's
shared resource budget and its current allocation kinds. Resource counters
cover only allocations routed through that budget; they must not be added to
ESP heap usage or treated as the full app memory total. The query does no
work in the frame or audio callback.

The `surface` scope separately counts ESP heap blocks owned by the Surface
backend: frame buffers, raster scratch, DrawList mailboxes and the optional
performance probe. These allocations are outside the shared resource budget
but inside ESP heap usage, so do not add them to either heap total. `peak_total`
is the Surface-owned high watermark since firmware boot, including short
mailbox replacement overlap; it is not an app-local whole-device RAM peak.

Test-control firmware advertising `perf-raster` also accepts the raw PXADB
commands `PERF START`, `PERF STOP`, `PERF READ raster|present OFFSET`, and
`PERF CLEAR`. The bounded 256-frame probe allocates PSRAM only while enabled;
read samples after stopping it. Raster samples exclude panel transfer, while
`present` samples are intervals between completed new GameRender frames on
boards with a panel-completion hook. `tools/measure-device-voxel.py` runs the
warmup, capture, sample download, screenshot and cleanup for an already
installed signed Voxel package. The optional `--package` argument records a
local package file hash in the report; install that same file first when using
this option. Use `--menu-tap X Y` for interactive builds that start at a menu.
When the firmware also advertises `memory`, the report includes ESP heap and
shared-budget snapshots before launch, after warmup, after frame capture and
after stop. `tools/compare-device-voxel-memory.py` validates and compares an
old/new/old sequence; its heap deltas are sampled steady-state differences,
not independent per-app peak measurements.

Firmware advertising `perf-fps` supports `pxadb perf` (`PERF FPS` on the wire),
which reads completed presentation FPS without scanning storage or enabling a
per-frame probe. Use it for live sampling. `INFO` also reports filesystem
capacity and can traverse LittleFS under its lock; polling it changes the I/O
workload and is unsuitable for gameplay FPS measurements.

Restart a connected device with `pxadb reboot --port /dev/ttyACM0`. Boards that
register a controllable power latch also advertise `poweroff` and accept
`pxadb poweroff --port /dev/ttyACM0`; unsupported boards report that software
power-off is unavailable rather than restarting.

If Host initialization fails (for example, storage is full), the firmware
shows a recovery screen and starts PXADB for that boot without changing the
saved diagnostics preference. Back up files with `fs pull` before cleanup.
After `fs unlock`, `fs rm` may remove individual `.part` files under
`pxa-state/data/` while the Host is unavailable. This exception does not permit
recursive deletion, uploads, completed private files or installed packages,
and is disabled once the Host initializes. Finish with `fs lock` and `reboot`;
recovery never automatically deletes files or formats storage.

Capture the final visible frame with `pxadb screenshot`. Firmware that
advertises the `screenshot-jpeg` capability also accepts `--jpeg`, which asks
the device to encode the frame on-board (about 10 KB instead of a 142 KB
RGB565 burst):

```sh
pxadb screenshot capture.png --port /dev/ttyACM0
pxadb screenshot --jpeg capture.jpg --port /dev/ttyACM0
pxadb screenshot --jpeg capture.jpg --after-present --port /dev/ttyACM0
```

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

A desktop GUI built on this same client is available as `tools/pxadb-gui.sh`;
see [PXADB GUI](../pxadb-gui/README.md) for live preview, remote control, file
transfer and package management.

## 简体中文

多点触摸调试先查询 `input capabilities`。pai-touch 和产品模拟器支持两个独立
接触点；其他板卡继续按实际实现报告数量。协议为
`INPUT POINTER <DOWN|MOVE|UP|CANCEL> x y id`，`INPUT CANCEL` 取消全部接触点，
KEY 在切换界面前也取消所有注入手势。CANCEL 不应触发点击或射击。
连续手势应复用一个 `PxaDbClient` 连接；每指必须先 DOWN，独立 MOVE，最后 UP
或 CANCEL。设备编号不合法、重复 DOWN 或没有 DOWN 的 MOVE 会被拒绝。

GameKit 的 `input capabilities` 另外报告 `controller_mask=4095`。
`INPUT CONTROLLER 0 connected buttons` 注入完整12位手柄状态，connected 为0或1，
断开时 buttons 必须为0。按钮位与 PXA SDK 的 `PXA_CONTROLLER_*` 相同。
板卡在正常50ms上报路径中交付这些状态；实体按键优先，实体接管、CANCEL、KEY、
连接关闭或5秒无输入都会撤销注入。长按测试应复用连接并每200ms刷新状态。
未实现此回调的板卡报告 mask=0，不能据此假设支持蓝牙或USB手柄驱动。

```sh
# 按住 A 半秒，结束时自动释放；0x10 是 A。
PXADB_BAUD=2000000 pxadb input controller 0x10 --hold-ms 500 --port /dev/ttyACM0
```

模拟器控制 socket 的内部命令为 `TOUCH phase id x y` 和 `TOUCH CANCEL_ALL`，
两根手指对应独立 LVGL 输入设备。`POINTER phase x y` 保留单鼠标 SDL 测试路径。
pai-touch 的注入复用两个物理触点设备，调试期间暂停控制器采样，最后释放被 LVGL
读到后恢复；正在进行的实体手势不会被调试注入抢占。这里验证驱动后的游戏链路，
不包含实体触摸传感器采样延迟。

PXADB 是本工作区固件 PXADB USB Serial/JTAG 服务的主机客户端。它可安装独立构建的
PXA 容器，不会重新编译或烧录固件。

新固件可运行 `pxadb memory --port /dev/ttyACM0`，Korvo 另加
`--baud 2000000`。JSON 分列 ESP SRAM/PSRAM 的容量、空闲和历史最低空闲，
以及 PXA 共享资源预算的当前用量、峰值和分配类别。预算用量已经包含在堆占用中，
不能重复相加；未走共享预算的 LVGL、codec 和任务存储也不能据此称为零。

```sh
python3 -m pip install -e tools/pxadb

# 安装一个独立构建的 PXA App。
pxadb package install local/app-output/pai-touch/pxa-example.pxa \
  --port /dev/ttyACM0

# 查看已安装的 Package catalog。
pxadb package list --port /dev/ttyACM0

# 不经过启动器，直接启动或停止已安装的 App。
pxadb package run pxa-example --port /dev/ttyACM0
pxadb package stop pxa-example --port /dev/ttyACM0
```

客户端将本地 `.pxa` 暂存到固件 inbox，请求固件校验签名和文件清单，再通过常规
Package 事务提交。传入 `--yes` 可进行非交互式替换。只有一个可识别的 PXADB USB 设备时，
可以省略 `--port`。

可通过 `pxadb reboot --port /dev/ttyACM0` 重启设备。具有可控电源锁存的板子会声明
`poweroff` 能力，并支持 `pxadb poweroff --port /dev/ttyACM0`；不支持的软件关机板子
会明确提示不可用，不会被重启替代。

`pxadb screenshot` 抓取最终显示帧。声明 `screenshot-jpeg` 能力的固件还接受
`--jpeg`，由设备端直接编码 JPEG（约 10 KB，而 RGB565 突发是 142 KB）：

```sh
pxadb screenshot capture.png --port /dev/ttyACM0
pxadb screenshot --jpeg capture.jpg --port /dev/ttyACM0
pxadb screenshot --jpeg capture.jpg --after-present --port /dev/ttyACM0
```

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

基于同一客户端的桌面 GUI 位于 `tools/pxadb-gui.sh`，支持实时预览、远程操纵、
文件传输和 Package 管理，详见 [PXADB GUI](../pxadb-gui/README.md)。

Protected system directories can be created only after explicit `fs unlock`; this includes missing `system/pxa` and `system/fonts` parents for initial factory provisioning. Upload/replacement/removal still use the stricter policy. Finish provisioning with `fs lock`. A normally installed store cannot obtain installer authority merely by using the `pxa-store` ID; the product must provision the trusted factory package.
