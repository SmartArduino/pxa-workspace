# 板级 Component 模板

[English](README.md)

将此目录复制为 `firmware/boards/<board-id>/`。目录本身就是由 `PXA_BOARD` 选择的
ESP-IDF component。

保持板级 component 自包含：

- `idf_component.yml` 只声明该物理板子需要的依赖。
- `board.cmake` 声明该板子使用的 ESP 目标芯片。
- `sdkconfig.defaults` 与 `partitions.csv` 定义板级构建和分区布局。其中应定义一个
  `data/littlefs` PXA 数据分区，并在 `CONFIG_PXA_STORAGE_PARTITION_LABEL` 中写入完全
  一致的标签，同时配置绝对路径 `CONFIG_PXA_MOUNT_POINT`。
- `include/pxa_board.h` 导出 `pxa_board_register_selected()`。
- `src/` 存放驱动以及 `pxa_board_port_t` 的实现。

除非每个支持的板子都需要，否则不要把板级依赖加入
`firmware/main/idf_component.yml`。

PXA 数据存储不是固定名为 `assets` 的分区，也不会随常规固件更新烧录。通过 pxadb 开发
安装与出厂镜像生成参见 [PXA 应用交付](../../docs/zh-CN/pxa-app-delivery.md)。
