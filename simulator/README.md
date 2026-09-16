# Simulator / 模拟器

[简体中文](#简体中文)

## Current desktop simulator

`tools/simulator.sh ui` starts the imported `pxa-system` SDL/LVGL standard-UI
simulator. It is intentionally a fast host tool for layout, locale, theme,
input semantics and workspace display profiles.

```sh
# Starts with simulator/profiles/generic.toml.
tools/simulator.sh ui

# Selects a board-shaped display profile.
tools/simulator.sh ui --profile pai-touch

# Configure or build without starting a window.
tools/simulator.sh ui configure --profile pai-touch
tools/simulator.sh ui build --profile pai-touch
```

Profiles may set `corner_radius`, `safe_insets`, and `shape_background`. The
last value controls only the visible host backdrop outside a rounded panel;
saved PNG screenshots keep those pixels transparent.

`--app-root <root>` selects an external source tree containing
`<app-id>/package.json` files. These manifests create launcher entries only.
The UI simulator does not execute those cards, install `.pxa` containers or
provide a pxadb endpoint.

## Product simulator

`product` mode runs a signed package directory through the real WAMR Component
engine and host services. It is the desktop entry point for Guest App debugging:

```sh
tools/app.sh build arcade --target simulator --source-root deps/pxa-system/apps/pxa
tools/simulator.sh product --profile pai-touch \
  --package local/app-output/pai-touch/pxa-arcade \
  --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der
```

The package is verified before the `linux-x86_64` AOT artifact is selected and
started. The runner supplies window, LVGL UI, clock, silent development audio,
and permission services. It is a package runner, not a hardware emulator.

`--package` remains useful for one-off runs. `tools/simulator.sh run --profile
pai-touch` starts a local PXADB2 service automatically and stops that service
when its simulator window closes. It prints the socket, state directory and log
path before opening the simulator. Start the service separately when it should
remain available after the window closes:

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-arcade.pxa --simulator pai-touch
pxadb package list --simulator pai-touch
```

`pxadb devices` lists the running profile as `simulator:pai-touch`.
When a matching Package is installed, selecting its catalog card replaces the
standard-UI process with the product runner. The previous window closes before
the App window opens, so the simulator does not run two SDL windows at once.
`pxadb package run pxa-arcade --simulator pai-touch` remains available for
direct launch.

The service speaks PXADB2: length-prefixed binary frames keep package bytes
binary and use up to 64 KiB per upload frame. By default it is bound only to a
per-user Unix socket (`/tmp/pxa-simulator-<uid>/<profile>.sock`), while package
state is persistent and ignored at `local/simulator/<profile>/`. It stages the
`.pxa` in its inbox, then uses the POSIX installer to verify its signature and
commit the normal slot transaction. Use `tools/simulator.sh service stop
--profile pai-touch` to stop it. `PXA_SIMULATOR_SOCKET_ROOT` overrides the
socket parent when needed.

Multiple `run --profile pai-touch` windows share this service and installed
state. The automatically started service remains available until the last such
window closes. An explicit `service start` is persistent and must be stopped
explicitly.

For an explicitly enabled trusted-LAN development listener, the service creates
a mode-0600 token file and requires it on every TCP connection:

```sh
tools/simulator.sh service start --profile pai-touch --listen 0.0.0.0:9222
pxadb package install local/app-output/pai-touch/pxa-arcade.pxa \
  --connect 192.168.1.20:9222 \
  --token local/simulator/pai-touch/pxadb2.token
```

TCP is intended for a trusted development LAN. It uses token authentication but
does not yet encrypt traffic; do not expose it beyond that network. See
[PXADB2 Binary Transport](../docs/pxadb2.md) for framing details.

The standard-UI simulator exposes PXADB through this service; drag-and-drop
installation is not implemented.

PXADB can also inspect and drive the visible simulator frame. These commands
work for both the standard UI and an installed App after it takes over the
window:

```sh
pxadb screenshot simulator.png --simulator pai-touch
pxadb input tap 148 120 --simulator pai-touch
pxadb input swipe 40 120 250 120 --duration-ms 300 --simulator pai-touch
pxadb input touch down 148 120 --simulator pai-touch
pxadb input touch move 168 120 --simulator pai-touch
pxadb input touch up 168 120 --simulator pai-touch
pxadb input key back --simulator pai-touch
```

`screenshot` returns a PNG of the final LVGL frame. Pointer coordinates are
logical display pixels; supported keys are `back`, `home`, `volume-up`, and
`volume-down`. `pxadb run scenario.pxauto --simulator pai-touch` can combine
the same actions with `wait` and `screenshot` steps for repeatable debugging.

For a connected device, build the independent package with
`tools/app.sh build <app-id> --board <board>` and install that `.pxa` with
pxadb. The device stages it in its inbox and applies the normal signed-package
installation transaction.

## 简体中文

## 当前桌面模拟器

`tools/simulator.sh ui` 启动引入的 `pxa-system` SDL/LVGL 标准 UI 模拟器。它是一个用于
快速验证布局、多语言、主题、输入语义和工作区显示 Profile 的主机工具。

```sh
# 使用 simulator/profiles/generic.toml。
tools/simulator.sh ui

# 选择与板子显示形态一致的 Profile。
tools/simulator.sh ui --profile pai-touch

# 只配置或构建，不启动窗口。
tools/simulator.sh ui configure --profile pai-touch
tools/simulator.sh ui build --profile pai-touch
```

Profile 可设置 `corner_radius`、`safe_insets` 和 `shape_background`。最后一个值只控制
圆角面板外的主机可见背景；保存的 PNG 截图会将这些像素保留为透明。

`--app-root <root>` 选择外部源码树；其中需要包含 `<app-id>/package.json`。这些
manifest 只会生成启动器条目。UI 模拟器不会执行这些卡片，也没有 `.pxa` 安装或 pxadb
端点。

## 产品模拟器

`product` 模式会通过真实 WAMR Component engine 和 Host service 执行已签名的 Package
目录，是调试 Guest App 的桌面入口：

```sh
tools/app.sh build arcade --target simulator --source-root deps/pxa-system/apps/pxa
tools/simulator.sh product --profile pai-touch \
  --package local/app-output/pai-touch/pxa-arcade \
  --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der
```

启动前会验签并选择 `linux-x86_64` AOT Artifact。运行器提供窗口、LVGL UI、时钟、静音的
开发音频和权限服务。它是 Package 运行器，而不是硬件模拟器。

`--package` 仍适用于一次性运行。`tools/simulator.sh run --profile pai-touch` 会在启动
模拟器前自动启动 PXADB2 服务，并在模拟器窗口关闭时停止这个自动启动的服务；它会打印
socket、状态目录和日志路径。需要窗口关闭后仍保持服务时，可单独启动服务：

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-arcade.pxa --simulator pai-touch
pxadb package list --simulator pai-touch
```

`pxadb devices` 会将运行中的 Profile 列为 `simulator:pai-touch`。
当同名 Package 已安装时，选择它的 catalog 卡片会由 product runner 接管标准 UI 进程。
旧窗口会先关闭，再打开 App 窗口，因此不会同时存在两个 SDL 窗口。`pxadb package run
pxa-arcade --simulator pai-touch` 仍可用于直接启动。

服务使用 PXADB2：定长二进制 frame 保留 Package 原始字节，单个上传 frame 最大为 64 KiB。
默认只绑定每个用户的 Unix socket，位置为
`/tmp/pxa-simulator-<uid>/<profile>.sock`；Package 状态持久保存于被忽略的
`local/simulator/<profile>/`。它将 `.pxa` 暂存到 inbox 后，使用 POSIX installer
完成验签和常规 slot 事务提交。用
`tools/simulator.sh service stop --profile pai-touch` 停止服务；需要时可通过
`PXA_SIMULATOR_SOCKET_ROOT` 修改 socket 父目录。

同一 `run --profile pai-touch` 可同时打开多个窗口，它们共享服务和已安装状态。自动启动的
服务会持续到最后一个窗口关闭；显式执行的 `service start` 会常驻，必须显式停止。

需要在受信任局域网中调试时，可显式开启 listener；服务会创建权限为 0600 的 token 文件，
每个 TCP 连接都必须携带该 token：

```sh
tools/simulator.sh service start --profile pai-touch --listen 0.0.0.0:9222
pxadb package install local/app-output/pai-touch/pxa-arcade.pxa \
  --connect 192.168.1.20:9222 \
  --token local/simulator/pai-touch/pxadb2.token
```

TCP 仅用于受信任开发局域网。它使用 token 认证，但当前不加密流量，不能暴露到更广网络。frame
细节参见 [PXADB2 Binary Transport](../docs/pxadb2.md)。

标准 UI 模拟器通过此服务提供 PXADB 端点；SDL 文件拖放安装仍未实现。

PXADB 也可以直接查看和驱动当前模拟器的可见画面。无论标准 UI 还是已安装 App 接管窗口后，
以下命令都可用：

```sh
pxadb screenshot simulator.png --simulator pai-touch
pxadb input tap 148 120 --simulator pai-touch
pxadb input swipe 40 120 250 120 --duration-ms 300 --simulator pai-touch
pxadb input touch down 148 120 --simulator pai-touch
pxadb input touch move 168 120 --simulator pai-touch
pxadb input touch up 168 120 --simulator pai-touch
pxadb input key back --simulator pai-touch
```

`screenshot` 返回最终 LVGL 帧的 PNG；指针坐标使用逻辑显示像素。支持的按键为 `back`、
`home`、`volume-up` 和 `volume-down`。`pxadb run scenario.pxauto --simulator pai-touch`
可用相同动作配合 `wait`、`screenshot` 构成可重复的调试场景。

连接真实设备时，先用 `tools/app.sh build <app-id> --board <board>` 独立生成包，再用
pxadb 安装 `.pxa`。设备会将其放入 inbox，并执行常规的已签名 Package 安装事务。
