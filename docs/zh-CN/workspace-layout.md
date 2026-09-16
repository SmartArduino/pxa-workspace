# 工作区结构

[English](../workspace-layout.md)

工作区有四条彼此独立的开发链路：

| 目录 | 负责内容 | 不负责内容 |
| --- | --- | --- |
| `firmware/` | ESP-IDF 产品、板子、驱动与 pxadb 服务 | PXA App 源码和出厂镜像生成 |
| `factory/` | 受版本控制的板级出厂预置选择与静态基础内容 | 开发者本机源码路径 |
| `simulator/` | 桌面显示与输入 Profile | ESP-IDF 驱动、DMA 与物理外设 |
| `local/` | 私有 App catalog、临时出厂覆盖和输出 | 任何需要追踪的产品策略 |

## 板级开发

将板子放在 `firmware/boards/<id>/`。该 component 拥有驱动、`idf_component.yml`、
`sdkconfig.defaults`、`partitions.csv` 以及 `pxa_board_port_t` 实现。使用统一入口，
每块板子都会有独立构建目录：

```sh
tools/firmware.sh pai-touch build
tools/firmware.sh pai-touch flash --port /dev/ttyACM0 --baud 406800
tools/firmware.sh pai-touch monitor --port /dev/ttyACM0 --baud 115200
```

不要将板级驱动放进通用 PXA component。已有产品可保持原有代码结构，仅在板级代码旁
添加 adapter。

## App 开发

PXA App 源码通常位于本工作区外，例如独立的 App 仓库。在被忽略的
`local/apps.toml` 中将 App ID 映射到其父源码目录，然后无需 ESP-IDF 即可打包：

```sh
tools/app.sh build pai-touch-diagnostics --board pai-touch
```

生成的 `.pxa` 用于 pxadb 安装。它不是固件依赖，也不会被常规固件构建自动包含。

## 出厂置备

`factory/profiles/<board>.toml` 保存某块板子可复现、受版本控制的 App 列表和基础内容；
实际源码路径保持在本机。临时或私有 App 放到被忽略的 overlay，并传给
`tools/factory.sh image`。出厂镜像会替换整个 PXA 数据分区；日常 App 迭代应使用 pxadb。

## 模拟器

使用 `tools/simulator.sh` 验证标准 UI 布局、鼠标/触摸输入与显示 Profile；未指定时使用
`generic`，其他 Profile 通过 `--profile` 选择。可指定外部 App manifest catalog：

```sh
tools/simulator.sh --profile pai-touch --app-root /work/pxa-apps
```

它刻意不是硬件模拟器。显示控制器初始化、DMA、触控 IC、音频和电源必须通过固件与对应真实
板子验证。真实 Guest 执行可先用 `tools/app.sh build <app-id> --target simulator` 独立打包，
再用 `tools/simulator.sh product --package <目录>` 运行已签名的解包目录；也可走持久化的
本地 PXADB 流程：

```sh
tools/simulator.sh service start --profile pai-touch
pxadb package install local/app-output/pai-touch/pxa-<app-id>.pxa --simulator pai-touch
tools/simulator.sh product --profile pai-touch --installed pxa-<app-id>
```

安装状态位于被忽略的 `local/simulator/<profile>/`；详情见[模拟器](../../simulator/README.md)。
