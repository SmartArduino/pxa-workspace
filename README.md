# PXA ESP 平台工程

[English](README.en.md)

这是一个独立的 ESP-IDF 产品工程。它以依赖方式引入 `pxa-system`，不会依赖旧产品
固件目录。通过同一份 `pxa_board_port_t` 合约支持两种板级接入方式：

- 新 BSP 在 `firmware/boards/<board>/` 中直接实现 board port。
- 已有固件在其显示初始化完成后，增加一个小型 adapter，将既有板级 API 映射为
  board port。

## 只开发应用？先用 DevKit

无需克隆本仓库或安装 ESP-IDF。下载 [DevKit 0.4.0](https://pxa.doit.am/docs/downloads/)，解压后：

```sh
export PATH="$PWD/pxa-devkit-0.4.0-linux-x86_64/bin:$PATH"
pxa init hello
pxa run hello
```

默认 C++26、O3，项目通过 `pxa.lock` 固定 SDK 与工具链版本。当前发行包要求 Linux x86_64、glibc 2.39+。应用包需要 Core 2.0 及其声明的服务能力。
[中文入门教程](https://pxa.doit.am/docs/first-app/) · [C++ SDK](deps/pxa-system/sdk/guest-cpp/README.zh-CN.md) · [发布说明](https://github.com/SmartArduino/pxa-system/releases/tag/v0.4.0) · [本次发布验证](docs/release-0.4.0.zh-CN.md)

可选的 [PXA AI 套件](https://pxa.doit.am/docs/ai-development/) 提供开发、调试、真机验收与发布 skills，以及项目内 agent 配置和自动验证脚本。配合现有 DevKit 使用，不增加设备运行开销。[实际验证报告](docs/ai-kit-validation-20261014.zh-CN.md)

Windows/macOS 可使用 [跨平台工具](tools/portable/README.md)：容器负责构建和无界面模拟器，宿主机 PXADB 直接连接 USB 串口；无需 USB 转发。

以下命令面向修改系统或板级驱动的开发者，在本仓库根目录运行：

```sh
git clone --recurse-submodules https://github.com/SmartArduino/pxa-workspace.git
cd pxa-workspace
git clone https://github.com/SmartArduino/pxa-apps.git local/pxa-apps
```

## 目录

```text
firmware/                    唯一的 ESP-IDF 工程
firmware/boards/             每块板子的驱动、默认配置和分区表
simulator/                   桌面 UI/显示 Profile 模拟配置
factory/                     可复现的板级出厂预置 Profile
tools/                       固件、应用、模拟器和出厂工具入口
local/                       忽略的本机 App catalog、覆盖配置与输出
deps/pxa-system/             引入的上游系统与应用打包工具
```

`firmware/boards/pai-touch` 本身就是 ESP-IDF component。通过 CMake cache 中的 `PXA_BOARD`
选择板子，无需维护一个全局 `CONFIG_BOARD_TYPE` choice。该目录下的
`sdkconfig.defaults` 会在根目录的跨板默认配置之后作为板级 overlay 加载。

## 托管依赖

ESP-IDF 依赖由拥有对应代码的目录声明。产品基线在 `firmware/main/idf_component.yml`；每块板子的
显示、输入、电源、联网和可选字体包写在 `firmware/boards/<board>/idf_component.yml`。这样即使板子
使用不兼容版本的驱动，也能保持依赖图隔离。Component Manager 会在 `firmware/` 中生成
`dependencies.lock.<board>`；依赖解析后应提交对应板子的该文件，但绝不可手工编辑。

## 编译指定板子

通过板级工具编译与调试，并为每块板子使用独立构建目录。

`esp-mosaico` 配置适配 ESP32-S31、480 × 480 CO5300 显示、CST92xx 触摸、
ES8311 播放和原生 USB CDC。硬件版本识别、ESP-IDF main 环境与当前外设限制见
[板级说明](firmware/boards/esp-mosaico/README.md)。

`doit-gamekit`（doit游戏机 / DOIT GameKit）适配 ESP32-S31、320×240 ST7789、
12 个手柄键、MAX98357A 音频和 SPI SD 卡，无触摸。构建命令为
`tools/firmware.sh doit-gamekit build`；默认纯按键焦点导航，无底部导航栏，支持
兜底鼠标。横屏使用整帧软件旋转，按 ST7789 原生方向输出，不依赖 TE。
接线、按键操作和首次烧录见
[板级说明](firmware/boards/doit-gamekit/README.md)。

`doit-gamekit-1` 对应 V0.8 新板，采用参考工程的 ST7789V TN 横屏初始化、
FT6336U 触摸、14 个按键、MAX98357A 和原生四线 SDMMC，新增 MENU/RETURN
系统键。构建命令为 `tools/firmware.sh doit-gamekit-1 build`；接线、屏幕参数
和首次烧录见 [板级说明](firmware/boards/doit-gamekit-1/README.md)。

```sh
tools/firmware.sh pai-touch build
tools/firmware.sh pai-touch flash --port /dev/ttyACM0 --baud 406800
tools/firmware.sh pai-touch monitor --port /dev/ttyACM0 --baud 115200
```

等效的 ESP-IDF 直接构建命令为：

```sh
idf.py -C firmware -B build/firmware/pai-touch -DPXA_BOARD=pai-touch build
```

`PXA_BOARD` 会选择 `firmware/boards/<board>/`、其 `board.cmake`、`sdkconfig.defaults`、
分区表和 `idf_component.yml`。`board.cmake` 会在 component 发现前选择匹配的 ESP
目标芯片。第一次配置会解析该板子的托管 component，并生成
`firmware/dependencies.lock.<board>`。不要将同一个构建目录复用于另一块板子。

## PXA 应用交付

产品应用源码放在独立的 `local/pxa-apps` Git 仓库；本工作区忽略该目录。
`deps/pxa-system/apps/pxa` 保存开发签名夹具与部分历史示例；新应用使用 pxa init 生成的 C++26 模板。
工作区的 App 工具会自动发现 `local/pxa-apps`，也可显式传入 `--source-root`。

固件构建和 PXA 应用交付被刻意拆开。App 源码根目录优先由被忽略的 `local/apps.toml` 解析，
未配置时使用 `local/pxa-apps`；
私有 App 不会污染本工程。常规固件编译与烧录既不会打包应用，也不会写入 PXA 数据分区。
详见 [PXA 应用交付](docs/zh-CN/pxa-app-delivery.md)。
目录职责与日常流程见 [工作区结构](docs/zh-CN/workspace-layout.md)。

SD 资源访问、多分区、安装位置选择及跨卷迁移的现状与拟议 ABI/SDK，见
[多卷存储设计](docs/pxa-multi-volume-design.zh-CN.md)。授权 SD 只读接口已有先行实现；
整应用安装的统一 `/PXA` 布局，以及无需 Flash 文件分区的 SD 系统盘，见
[固定目录与 SD 系统盘设计](docs/pxa-storage-layout.zh-CN.md)（安装及系统盘后端尚未实现）。
默认资源根已支持无需选目录的 `fs.app_media()`，见
[SDK 示例](deps/pxa-system/sdk/guest-cpp/examples/fixed-files/README.zh-CN.md)与
[GameKit 首次启动/重启验证](docs/validation/2026-10-18-sd-fixed-root.zh-CN.md)。
Duke3D 已从固定 SD 目录自动读取资源并进入关卡，实测与边界见
[游戏真机验证](docs/validation/2026-10-18-duke3d-sd.zh-CN.md)。

## PXA 快速开发

`tools/dev.sh` 将应用的增量打包、PXADB 覆盖安装和启动串为一个开发循环。App 源码根目录是
包含 `<app-id>/` 的父目录：可在被忽略的 `local/apps.toml` 中登记，也可在命令中临时传入。

```toml
# local/apps.toml
[apps.my-app]
source_root = "/home/me/work/pxa-apps"
```

模拟器模式会安装应用，启动完整 PXA 系统（与 `tools/simulator.sh run` 相同），并自动进入该应用。
系统桌面、设置、输入法、主题、DPI 和屏幕形状都会生效。PXADB2 服务保持运行，
每次保存后重启系统并进入更新后的 App；Host 及 Guest 的 `pxa_log_*` 日志输出到终端：

```sh
tools/dev.sh sim my-app --watch
# 或者不登记本机 catalog：
tools/dev.sh sim my-app --source-root /home/me/work/pxa-apps --watch
# 使用 Watcher 的 412×412 圆屏配置：
tools/dev.sh sim --board sensecap-watcher pixel-dungeon
tools/dev.sh sim --board esp-mosaico voxel-craft
```

模拟器默认使用与 `--board` 同名的屏幕 profile；需要单独覆盖时传入 `--profile`。
设置和 App 数据与 `tools/simulator.sh run` 共用该 profile 的持久化目录；传入
`--instance <名称>` 可使用独立的开发实例。只需要单独运行应用时，可使用
`tools/simulator.sh product --profile <板型> --installed <包 ID>`。
`dev.sh` 默认只打包当前目标的 AOT，不附带 `.wasm` 回退文件（包括声明为 `artifact: "both"`
的组件）；应用中明确声明为
Wasm-only 的组件仍会保留。需要同时打包 Wasm 时，可直接使用 `tools/app.sh build`。

设备模式从 `firmware/boards/<board>/board.cmake` 的 `PXA_BOARD_TARGET` 读取芯片目标
（默认 pai-touch / esp32s3）。新增同架构板型只需声明该值，无需修改应用工具的板名映射。
`tools/app.sh` 与底层打包入口共用同一读取方式；显式 `--target` 可覆盖默认架构，
缺失声明时直接报错。设备模式覆盖安装到现有固件，不会重新烧录固件；部署后默认会显示
`pxadb logcat`，下一次更新前会自动暂时关闭它以释放 USB Serial/JTAG 连接：

```sh
tools/dev.sh device my-app --port /dev/ttyACM0 --baud 2000000 --watch
```

所有构建、部署和运行器输出同时保存在 `local/dev-logs/<mode>/<app-id>.log`。这是重新加载
WASM/AOT 应用的快速循环，而非运行时热重载，因此每次代码更新会重启运行环境。

## PXADB GUI

`tools/pxadb-gui.sh` 为 CLI 所用的同一个 `pxadb` 客户端提供桌面界面：发现 USB 设备与
正在运行的产品模拟器，把设备截图流式显示为实时预览，注入点击、拖动和按键，浏览并传输
文件，显示设备日志，管理 Package。预览在 USB 固件上通过设备端 JPEG 约 3~4 FPS，在产品
模拟器上约 15~20 FPS；拖动时会暂停截图采集，保证操控响应。

```sh
tools/pxadb-gui.sh
tools/pxadb-gui.sh --port /dev/ttyACM0
tools/pxadb-gui.sh --simulator pai-touch
```

PXADB 独占串口，请在打开 GUI 前关闭正在使用同一端口的 `pxadb` 命令或
`tools/dev.sh device`。详见 [PXADB GUI](tools/pxadb-gui/README.md)。

## 模拟器

`tools/simulator.sh` 启动引入的 SDL/LVGL 标准 UI 模拟器，未指定时使用默认的
`generic` Profile。其他板子 Profile 显式通过参数选择：

```sh
tools/simulator.sh --profile pai-touch
```

标准 UI 模式运行完整系统；已安装的桌面 Guest 应用可以从启动器打开。目录里只有 manifest 的卡片需要先构建和安装，不能当作应用已经运行的证据。

```sh
tools/dev.sh sim pixel-dungeon --source-root local/pxa-apps
# 只运行已安装应用，不显示系统桌面：
tools/simulator.sh product --profile pai-touch --installed pxa-pixel-dungeon
```

签名和目标选择与真机使用相同的包契约。模拟器用于布局、输入和 Guest 行为验证；硬件驱动、DMA、触控 IC、音频输出延迟与电源仍须在真机验证。详见[模拟器说明](simulator/README.md)。

## 板级合约

board port 提供显示创建与显示 Profile，以及可选的网络、音量、亮度、状态发布和诊断
控制。它只承载控制面：DMA 提交、framebuffer 所有权、像素转换和 PCM 帧传输仍留在
板级 component 中。pai-touch 的 PXA 直出帧路径就是这种边界的示例。

## 适配已有固件

不要迁移旧的板级实现。在原板子旁增加一个 adapter，注册 `pxa_board_port_t`，并在
既有 LVGL display 初始化完成后调用 `pxa_integration_start()`。adapter 独占对旧
`Board`、UI 和产品类的引用；`pxa-system` 与通用 integration component 不会包含
这些头文件。

## 依赖

`deps/pxa-system` 是独立 Git checkout，并固定 WAMR 和 page-manager 子模块。ESP
host 直接持有很小的 WAMR/ESP-IDF 声明兼容层。host 与板级依赖
均保存在本工程内，因此源码和 CMake 文件不包含原 pai-touch 演示固件的路径。
