# PXA ESP 平台工程

[English](README.md)

这是一个独立的 ESP-IDF 产品工程。它以依赖方式引入 `pxa-system`，不会依赖旧产品
固件目录。通过同一份 `pxa_board_port_t` 合约支持两种板级接入方式：

- 新 BSP 在 `firmware/boards/<board>/` 中直接实现 board port。
- 已有固件在其显示初始化完成后，增加一个小型 adapter，将既有板级 API 映射为
  board port。

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

通过板级工具编译与调试，并为每块板子使用独立构建目录：

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
`deps/pxa-system/apps/pxa` 仍包含系统示例应用和开发签名夹具。
工作区的 App 工具会自动发现 `local/pxa-apps`，也可显式传入 `--source-root`。

固件构建和 PXA 应用交付被刻意拆开。App 源码根目录优先由被忽略的 `local/apps.toml` 解析，
未配置时使用 `local/pxa-apps`；
私有 App 不会污染本工程。常规固件编译与烧录既不会打包应用，也不会写入 PXA 数据分区。
详见 [PXA 应用交付](docs/zh-CN/pxa-app-delivery.md)。
目录职责与日常流程见 [工作区结构](docs/zh-CN/workspace-layout.md)。

## PXA 快速开发

`tools/dev.sh` 将应用的增量打包、PXADB 覆盖安装和启动串为一个开发循环。App 源码根目录是
包含 `<app-id>/` 的父目录：可在被忽略的 `local/apps.toml` 中登记，也可在命令中临时传入。

```toml
# local/apps.toml
[apps.my-app]
source_root = "/home/me/work/pxa-apps"
```

模拟器模式会保持 PXADB2 服务运行，在每次保存后重启 App 进程，并将运行器及 Guest 的
`pxa_log_*` 日志输出到终端：

```sh
tools/dev.sh sim my-app --watch
# 或者不登记本机 catalog：
tools/dev.sh sim my-app --source-root /home/me/work/pxa-apps --watch
# 使用 Watcher 的 412×412 圆屏配置：
tools/dev.sh sim --board sensecap-watcher pixel-dungeon
```

模拟器默认使用与 `--board` 同名的屏幕 profile；需要单独覆盖时传入 `--profile`。

设备模式独立构建 `esp32s3` 包并覆盖安装到现有固件，不会重新烧录固件；部署后默认会显示
`pxadb logcat`，下一次更新前会自动暂时关闭它以释放 USB Serial/JTAG 连接：

```sh
tools/dev.sh device my-app --port /dev/ttyACM0 --baud 2000000 --watch
```

所有构建、部署和运行器输出同时保存在 `local/dev-logs/<mode>/<app-id>.log`。这是重新加载
WASM/AOT 应用的快速循环，而非运行时热重载，因此每次代码更新会重新启动该 App 窗口。

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

可通过 `--app-root <root>` 指定外部 App manifest 根目录。该 UI 模式刻意只用于 UI、输入和
显示 Profile 工作，其启动器卡片不会执行 Guest 字节码。包级 App 调试时，先构建桌面目标包，
再通过 `product` 模式运行已签名的 Package 目录：

```sh
tools/app.sh build pixel-dungeon --target simulator --source-root local/pxa-apps
tools/simulator.sh product --profile pai-touch \
  --package local/app-output/pai-touch/pxa-pixel-dungeon \
  --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der
```

产品模式会验签、选择 `linux-x86_64` WAMR AOT，并通过窗口、UI、时钟、音频和权限 Host
service 运行 Guest。需要持久化桌面安装时，先运行
`tools/simulator.sh service start --profile pai-touch`，再通过
`pxadb package install <app>.pxa --simulator pai-touch` 安装，并用
`tools/simulator.sh product --installed <app-id>` 启动。硬件驱动、DMA 时序、触控 IC、音频和
电源仍须在真实板子上验证。详见[模拟器](simulator/README.md)。

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
