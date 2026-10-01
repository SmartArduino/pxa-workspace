# pai-touch 板级示例

[English](README.md)

此目录本身是一个 ESP-IDF component，通过 `PXA_BOARD=pai-touch` 选择。它包含样板
硬件实现：JD9853 显示屏、CST826 触摸、ADC 按键、电池监测、Wi-Fi 与 JL701 音频。

扬声器由 JL701 协处理器经 UART 驱动，板子自身没有 codec。`src/jl701_audio_link.cc`
拥有这条链路（握手、保活、PCM 节拍与音量），只对外暴露
`include/jl701_audio_link.h` 中的小接口；`PaiTouchAudio` 把它接成 PXA 音频输出，
它依赖的 RPC 库则以私有形式存放在本 component 的 `vendor/rpc701/` 下。因为没有任何
共享 component 依赖它，所以不再放进 `firmware/components/`。

以下是在真机上量到的行为，链路就是按这些行为设计的：

- 协处理器在 `keep_alive_ms`（4 秒）内收不到心跳就会断开并休眠，所以链路每 2 秒发一次
  心跳；需要唤醒时把 TX 拉低 1 秒再重新握手，且只在链路持续不通时才重新拉低，避免每次
  重试都打断它的启动。
- 唤醒脉冲之后立刻发的第一次握手会丢，因为协处理器还在启动；几秒后的重试才成功。所以
  开机日志里可能出现 "still booting"，随后音频才可用。
- 握手里保持远端功放控制关闭：让协处理器去驱动它自己的 PA 脚会让它彻底不应答，厂商参考
  固件同样保持关闭，两种情况扬声器都能出声。
- 协处理器会无流控地持续推送麦克风 Opus 帧。链路只负责收下并丢弃内容，因此唤醒后那阵
  突发导致的接收溢出最多每 10 秒上报一次。

CST826 的两个触点分别映射为独立的 LVGL 指针设备，PXA 应用因此能收到稳定的
`pointer_id`，从而支持双指同时触摸。

Wi-Fi 获取 IP 地址后，板子会通过 `pool.ntp.org` 校准系统时间。ESP-IDF SNTP
服务在联网期间定期更新时间，重新联网后也会再次启动校时。

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
