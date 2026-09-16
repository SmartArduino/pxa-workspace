# PXA ESP-IDF 平台组件

[English](README.md)

本组件是产品固件到 PXA System 的 ESP-IDF 适配层。规范、可移植核心、Guest
SDK、应用和打包工具都属于 `../../../deps/pxa-system`；这里不复制协议实现，只负责设备
生命周期、ESP 服务、Package 持久化以及产品 UI 连接。

## 目录边界

- `include/pxa/`：稳定的产品侧 C API。
- `src/runtime/`：启动、Package 激活、宿主线程、事件与输入调度。
- `src/services/`：Audio、Net、Surface 以及激活期服务注册。
- `src/package/`：发布者信任、Package 仓库、安装策略、权限和图标目录。
- `src/ui/`：中立 UI Shell、资源缓存和 `app_pages` 适配。
- `tests/`：ESP 适配状态机和后端边界的宿主机单测。

不调用 ESP-IDF 即可运行这些检查：

```sh
firmware/components/pxa/tests/test_host.sh
```

可移植 C99 实现继续命名为 `libpxa`，位于
`../../../deps/pxa-system/libpxa`。ESP 组件只通过其公开 API 和明确的 adapter 接口使用
它，不向 `libpxa` 引入 ESP-IDF、FreeRTOS、LVGL 产品页面或产品配置。

## Package 布局

出厂 Package 位于 `CONFIG_PXA_MOUNT_POINT/CONFIG_PXA_BUILTIN_PACKAGE_ROOT`。可变安装
目录使用发布者根摘要和 App ID 组成的身份：

```text
<mount>/<state-root>/packages/<publisher-root-hex>~<app-id>/
<mount>/<state-root>/packages/.session-<publisher-root-hex>~<app-id>/
<mount>/<state-root>/data/
<mount>/<state-root>/inbox/
```

启动、显式扫描、PXADB 提交/删除和安装操作会刷新快照。出厂 Package 直接在
产品拥有的目录中运行；Inbox 内容必须经过签名和清单摘要验证后才能安装。应用身份由
发布者和 App ID 共同确定，不能只依赖 App ID。

固件构建不会自动打包或烧录 PXA 应用。使用 `tools/pxa/package_app.sh` 生成供 pxadb
安装的 `.pxa`；仅在出厂置备时使用分区镜像工具。完整流程见
[PXA 应用交付](../../docs/zh-CN/pxa-app-delivery.md)。

## WAMR 集成

WAMR 源码是 `pxa-system/wamr` 中固定提交的子模块。ESP 构建只引用该源码，另由本
component 提供一个很小的 ESP-IDF 声明兼容头；不生成 overlay、不应用补丁、也不替换
WAMR 源文件。固定提交和 PXA 自身的 engine ABI 标识位于
`pxa-system/config/wamr.json`。

## 模拟器

- `../../../deps/pxa-system/simulator/desktop`：独立 SDL2/LVGL 标准系统 UI 模拟器。
- `../../../simulator`：本工程的显示与输入 Profile；不模拟硬件驱动或 Guest 执行。

协议和宿主架构详见 `../../../deps/pxa-system/docs/zh-CN/`；ESP 集成细节详见
`../../../deps/pxa-system/platforms/esp-idf/README.zh-CN.md`。
