# 新增板子

[English](../adding-a-board.md)

创建 `firmware/boards/<id>/` 作为一个 ESP-IDF component，目录至少包含 `CMakeLists.txt`、
`idf_component.yml`、`src/` 与 `include/`。实现并注册一个 `pxa_board_port_t`
实例。

添加 `board.cmake`，设置 `set(PXA_BOARD_TARGET "...")` 与
`set(PXA_BOARD_COMPONENT "...")`。`PXA_BOARD_COMPONENT` 是导出
`pxa_board.h` 的 ESP-IDF component，通常与板子目录名相同。根工程会在 ESP-IDF
发现 component 前读取这些元数据，使选中的板子、工具链和依赖解析器始终使用同一目标芯片。

所有板子都需要的软件包放在 `firmware/main/idf_component.yml`。显示、触控、编解码器、
蜂窝网络、电源等物理板级软件包，只放在 `firmware/boards/<id>/idf_component.yml`。ESP-IDF
只会处理已选中 component 的 manifest，因此未选中板子不会进入当前依赖图。根目录
自动生成的 `dependencies.lock.<board>` 应提交到版本控制，但绝不可手工修改。

必需 callback 为 `initialize`、`display` 与 `display_profile`。仅有显示能力的板子
也可以启动系统。网络、音量/亮度控制、`system_ready` 与诊断接口按硬件能力选择性
实现。

高频渲染必须留在板级层。例如板级 presenter 可以直接获取 PXA surface 并提交给 SPI
DMA；不要通过 `pxa_board_port_t` 复制帧，也不要通过通用 callback 逐像素、逐音频采样
或逐 DMA chunk 转发数据。

对已有产品，board port 可以是与原板级源码并列的 `legacy_board_adapter.cc`。它包装
已有公开 API，不要求重组原驱动或业务代码。

每个板子都导出 `include/pxa_board.h` 和 `pxa_board_register_selected()`。产品 `main`
仅调用这个稳定入口，因此通过 `-DPXA_BOARD=<id>` 选择板子时无需修改应用入口。

在板子的 `partitions.csv` 中定义一个 `data/littlefs` 分区，再在板级
`sdkconfig.defaults` 中设置完全一致的 `CONFIG_PXA_STORAGE_PARTITION_LABEL` 与绝对路径
`CONFIG_PXA_MOUNT_POINT`。这是板级属性，不应固定为 `assets`。可通过
`CONFIG_PXA_BUILTIN_PACKAGE_ROOT` 选择产品自有的可选出厂 Package 根目录。固件构建不会
创建或烧录此分区；通过 pxadb 开发交付与出厂镜像生成详见
[PXA 应用交付](pxa-app-delivery.md)。只为显示与输入语义新增
`simulator/profiles/<id>.toml`，不要将板级驱动复制进模拟器。
