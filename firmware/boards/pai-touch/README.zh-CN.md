# pai-touch 板级示例

[English](README.md)

此目录本身是一个 ESP-IDF component，通过 `PXA_BOARD=pai-touch` 选择。它包含样板
硬件实现：JD9853 显示屏、CST826 触摸、ADC 按键、电池监测、Wi-Fi 与 RPC701 音频。

CST826 的两个触点分别映射为独立的 LVGL 指针设备，PXA 应用因此能收到稳定的
`pointer_id`，从而支持双指同时触摸。

`sdkconfig.defaults` 与 `partitions.csv` 仅在选择该板子时加载。它们定义独立于固件
烧录的 `pxa_data` LittleFS 分区，并挂载至 `/pxa`。其中启用了 LVGL 的 LodePNG 解码器，
因为 PXA Package 使用 PNG 图标与 UI 资源。开发安装与显式出厂镜像生成参见
[PXA 应用交付](../../../docs/zh-CN/pxa-app-delivery.md)。

`idf_component.yml` 持有仅属于该板子的托管依赖：CST826 触控支持、按键、电池估算、
Wi-Fi 配网和 FreeType。跨板的 LVGL、LVGL port 与 LittleFS 依赖保留在
`firmware/main/idf_component.yml`。

`src/pai_touch_board_port.cc` 是唯一的 PXA 集成 sidecar：它将既有硬件对象映射为
`pxa_board_port_t`。`parallel_sw_rotation_flush.h` 继续拥有 PXA 直接帧提交和 SPI DMA
热路径，不会被通用层复制或转发。

component 在 `include/pxa_board.h` 中导出
`pxa_board_register_selected()`；`main` 使用该稳定名称，而不包含 pai-touch 专用头文件。
