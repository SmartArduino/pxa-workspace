# PXADB GUI

[简体中文](#简体中文)

A PySide6 desktop front end for the workspace `pxadb` client. It imports
`pxadb` as a library instead of reimplementing the protocol, so the GUI and
the CLI always share device discovery, screenshot decoding, input injection
and file transfer behavior.

```sh
# No install required inside the workspace.
tools/pxadb-gui.sh

# Optional standalone install.
python3 -m pip install -e tools/pxadb
python3 -m pip install -e tools/pxadb-gui
pxadb-gui --simulator pai-touch
pxadb-gui --port /dev/ttyACM0
pxadb-gui --port /dev/ttyUSB0 --baud 2000000
pxadb-gui --port /dev/ttyUSB1 --baud 921600  # EPDiy V7
pxadb-gui --connect 192.168.1.20:9222 --token local/simulator/pai-touch/pxadb2.token
```

## Features

- **Device picker** — USB Serial/JTAG devices, UART-transport boards behind a
  USB bridge, and running product simulators are probed on demand; the baud
  rate is selectable in the toolbar (or `--baud`). `--port`, `--simulator` and
  `--connect/--token` connect immediately at startup.
- **Reboot-tolerant connection** — when the device resets or re-enumerates on
  USB (the port name can change), the session detects the dead transport,
  releases it and reconnects automatically by matching the device serial
  number, so preview and panels recover without restarting the GUI.
- **Live preview** — the GUI requests `SCREENSHOT` frames on an interval and
  maps clicks and drags onto device coordinates. Firmware RGB565 frames render
  directly through Qt's `Format_RGB16`, so no PNG conversion runs per frame.
- **Remote control** — tap, drag, and Back/Home/Volume keys are injected
  through the firmware input router. Pointer MOVE events are coalesced in the
  GUI and again in the firmware input mailbox, and preview captures pause while
  the pointer is held down so drags stay responsive.
- **File transfer** — browse storage, create and delete paths, push and pull
  files with progress, preserving UTF-8 filenames (including Chinese names).
  Firmware paths are limited to 239 UTF-8 bytes including directory names;
  devices running the older ASCII-only path validator need a firmware update.
  Works on firmware (`FSLIST`/`FSREAD`/`FSSHA`) and on
  product simulators. On firmware, the Files panel can temporarily unlock
  writes under the internal Flash system directory for 1–1440 minutes and
  lock it again (`FSUNLOCK`/`FSLOCK`). SD card directories allow uploads,
  removal, directory creation and same-volume file moves without unlocking,
  except `sdcard/PXA/system`, which is read-only by default. Destructive operations
  on its parent `sdcard/PXA` are also denied. `FSUNLOCK` temporarily permits
  writes below `sdcard/PXA/system` as well; `FSLOCK` or expiry restores read-only
  access. The system directory itself stays in place.
- **Device log** — subscribe to structured logs and raw console output while
  the preview runs; the worker drains buffered output between requests.
- **Packages** — list installed packages, install a local `.pxa`, launch,
  stop, enable, disable, uninstall and clear data. On simulators *Run* starts
  the product runner through `tools/simulator.sh`.
- **Device actions** — reboot, software power-off and input sync.

## Expected preview rate

| Target | Screenshot | Preview rate |
| --- | --- | --- |
| USB Serial/JTAG, 296x240, device JPEG | ~0.08 s/frame (10 KB) | ~3-4 FPS |
| USB Serial/JTAG, 296x240, RGB565 fallback | ~0.5 s/frame (142 KB) | ~2 FPS |
| UART board, 800x480 @ 2 Mbaud, device JPEG | ~1.3 s/frame (14 KB) | ~0.8 FPS |
| UART board, 800x480 @ 2 Mbaud, RGB565 | ~7.9 s/frame (768 KB) | ~0.1 FPS |
| Product simulator, PNG over PXADB2 | ~0.05 s/frame | ~15-20 FPS |

The GUI requests `SCREENSHOT JPEG` whenever the firmware advertises the
`screenshot-jpeg` capability and falls back to RGB565 otherwise. Firmware also
rate-limits captures (default one per 250 ms), so JPEG preview reaches about
10 FPS only after lowering `CONFIG_PXADB_CAPTURE_MIN_INTERVAL_MS` (for example
to 100). The GUI never queues more than one preview request, so a connected
device is never flooded.

E-paper firmware advertising `screenshot-gray4-rle` uses a lossless 4-bit
grayscale preview ahead of JPEG. This keeps the panel's full logical resolution
without the large RGB565/JPEG staging allocations. The connection baud must
match the firmware; EPDiy V7 defaults to 921600. Enable **Live preview** or press
**Screenshot** before clicking on the preview.
On Linux the serial client holds its USB device open read-only while connected
to prevent runtime autosuspend from repeatedly disconnecting USB-UART bridges.

PXADB owns the serial port exclusively: close the GUI before running `pxadb`
or `tools/dev.sh device` against the same port, and vice versa.

## Tests

```sh
python3 -m pytest tools/pxadb-gui/test_pxadb_gui.py
```

## 简体中文

基于 PySide6 的 `pxadb` 桌面客户端。它把 `pxadb` 当作库来引入，而不是重新实现
协议，因此 GUI 与 CLI 的设备发现、截图解码、输入注入和文件传输行为始终一致。

```sh
# 工作区内无需安装。
tools/pxadb-gui.sh

# 也可以单独安装。
python3 -m pip install -e tools/pxadb
python3 -m pip install -e tools/pxadb-gui
pxadb-gui --simulator pai-touch
pxadb-gui --port /dev/ttyACM0
pxadb-gui --port /dev/ttyUSB0 --baud 2000000
pxadb-gui --port /dev/ttyUSB1 --baud 921600  # EPDiy V7
pxadb-gui --connect 192.168.1.20:9222 --token local/simulator/pai-touch/pxadb2.token
```

## 功能

- **设备选择** —— 按需探测 USB Serial/JTAG 设备、USB 桥后面的 UART 板以及正在运行的
  产品模拟器；波特率可在工具栏选择（或 `--baud`）。`--port`、`--simulator`、
  `--connect/--token` 可在启动时直接连接。
- **可容忍重启的连接** —— 设备复位或 USB 重新枚举（端口名可能变化）时，会话会
  检测到传输已失效、释放端口，并按设备序列号自动重连；预览和面板无需重启 GUI
  即可恢复。
- **实时预览** —— 按间隔请求 `SCREENSHOT`，把点击和拖动映射为设备坐标。
  固件 RGB565 帧通过 Qt `Format_RGB16` 直接显示，每帧不做 PNG 转换。
- **远程操纵** —— 点击、拖动以及 Back/Home/音量按键经固件输入路由注入。
  MOVE 事件在 GUI 侧和设备输入邮箱中各合并一次；按住指针时暂停预览采集，
  拖动响应保持在几十毫秒级。
- **文件传输** —— 浏览存储、新建/删除路径、带进度地上传下载，保留中文等 UTF-8
  文件名。固件路径（含目录）最多 239 个 UTF-8 字节；使用旧版 ASCII 路径校验的
  设备需要更新固件。固件
  （`FSLIST`/`FSREAD`/`FSSHA`）和产品模拟器都支持。连接固件时，文件面板可按
  1～1440 分钟临时解锁内部 Flash 系统目录写入，并可立即重新锁定
  （`FSUNLOCK`/`FSLOCK`）。SD 卡普通目录的上传、删除、新建目录和卡内文件移动
  无需解锁；`sdcard/PXA/system` 默认只读，父目录 `sdcard/PXA` 也禁止整体删除。
  `FSUNLOCK` 也可临时解锁 `sdcard/PXA/system` 内部文件和子目录，`FSLOCK` 或到期
  后恢复只读；系统根目录本身不能整体删除。
- **设备日志** —— 预览运行期间可订阅结构化日志与原始串口输出；worker 会在
  请求间隙排空缓冲区。
- **包管理** —— 列出已安装包、安装本地 `.pxa`、启动、停止、启用、禁用、
  卸载、清除数据。模拟器上 *Run* 会通过 `tools/simulator.sh` 启动产品运行器。
- **设备操作** —— 重启、软件关机、输入同步。

## 预览性能预期

| 目标 | 单帧截图 | 预览帧率 |
| --- | --- | --- |
| USB Serial/JTAG 296×240，设备端 JPEG | 约 0.08 秒/帧（10 KB） | 约 3~4 FPS |
| USB Serial/JTAG 296×240，RGB565 回退 | 约 0.5 秒/帧（142 KB） | 约 2 FPS |
| UART 板 800×480 @ 2 Mbaud，设备端 JPEG | 约 1.3 秒/帧（14 KB） | 约 0.8 FPS |
| UART 板 800×480 @ 2 Mbaud，RGB565 | 约 7.9 秒/帧（768 KB） | 约 0.1 FPS |
| 产品模拟器 PXADB2 PNG | 约 0.05 秒/帧 | 约 15~20 FPS |

固件声明 `screenshot-jpeg` 能力时，GUI 会请求 `SCREENSHOT JPEG`，否则回退到
RGB565。固件默认还有 250ms 的截图限频，因此把
`CONFIG_PXADB_CAPTURE_MIN_INTERVAL_MS` 调低（例如 100）后，JPEG 预览可接近
10 FPS。GUI 同时只保留一个预览请求，不会向设备堆压请求。

PXADB 独占串口：GUI 打开设备时，`pxadb` 命令与 `tools/dev.sh device` 会因端口被占用
而失败；反之亦然，请先关闭其中一方。

EPDiy V7 的运行波特率为 921600。连接后勾选 **Live preview**，或点
**Screenshot** 获取画面，再在画面上点击和拖动。支持 `screenshot-gray4-rle` 的
墨水屏固件会优先使用无损 4 位灰度预览，保留完整逻辑分辨率，避免大型 RGB565
和 JPEG 临时缓冲。
Linux 上串口客户端会在连接期间只读保持对应 USB 设备打开，避免 USB-UART 桥
因 runtime autosuspend 反复断开；断开连接时即释放，不修改系统电源策略。

## 测试

```sh
python3 -m pytest tools/pxadb-gui/test_pxadb_gui.py
```
