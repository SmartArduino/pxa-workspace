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
- 协处理器会无流控地持续推送麦克风 Opus 帧。PXA 采集源通过非阻塞读取取走完整帧，
  开关采集时清空旧帧。当前协处理器固件即使收到 20 ms 配置，实际仍输出
  单个 60 ms Opus 帧；采集格式按实际 60 ms 上报，避免误报或重新编码；没有采集者时由有限环形缓冲淘汰旧帧。唤醒后突发导致的
  UART 接收溢出最多每 10 秒上报一次。

CST826 的两个触点分别映射为独立的 LVGL 指针设备，PXA 应用因此能收到稳定的
`pointer_id`，从而支持双指同时触摸。

顶部性能浮层约每秒更新一次：`FPS` 统计完成 LCD 传输的帧数；`R` 是普通 UI 与游戏
直出帧的平均旋转、转换和相关合成耗时；`M` 是平均传屏耗时，包含等待 LCD 传输完成，
不含此前排队。两项耗时均以整数毫秒显示，不包含 Guest 更新和 Host 光栅化。
流水线各阶段可以重叠，不能用 `R + M` 换算帧率。关闭编译期性能日志也保留浮层统计；
没有新画面时，屏上保持最后一次采样值。
完成 pai-touch 构建后，可运行
`flock .pxa/firmware-build.lock python firmware/boards/pai-touch/tests/test_performance_build.py`
检查开启和关闭编译期性能日志的两种配置；无需修改 sdkconfig 或连接设备。

SPI 输出缓冲只在驱动确认排队的传输全部结束后复用。提交任务用
`esp_lcd_panel_io_tx_param(io, -1, nullptr, 0)` 排空 SPI 队列，不发送 LCD 命令；
绘制中途失败也先排空已提交的部分。固定超时不能证明 DMA 已停止，因此不再用
50 ms 的完成信号量超时归还缓冲。驱动异常时保留该帧，避免覆盖仍由 DMA 读取的像素。
这不增加帧缓冲，移除信号量后上下文减少 88 B；显示故障的完整实屏验收仍需单独确认。

桌面松手后直接选定目标页，以 160 毫秒吸附，避免惯性滑行后再次吸附；短距离快速
滑动也可翻页。图标和名称作为整体在卡片内垂直居中，删除按钮使用 40×40 像素触摸
区域和 32 像素圆形标记，滑动会取消删除点击。小屏图标省去阴影绘制；通知面板的时间和滑条数值原位更新，
避免状态刷新时重建桌面。PNG 解码缓存上限为 1 MiB，图像像素强制分配在 PSRAM，
缓存不会预先占满这部分内存。板级初始化也会设置缓存上限，兼容已有的 sdkconfig。
数据缓存行使用 64 字节；指令缓存保持 16 KiB，避免额外消耗紧张的内部 SRAM。
PXADB 文件读取暂存、权限列表暂存和商店下载记录放入 PSRAM；ESP32-S3 链接映射
显示内部静态 BSS 减少 10,992 字节。音频、Flash 操作相关任务栈和 ISR 缓冲仍留在内部 RAM。
已有 `firmware/sdkconfig.pai-touch` 的构建需另行将数据缓存行设为 64 字节，默认配置
不会覆盖保存过的选项。

桌面图标以按下位置为起点判定移动，超过随 DPI 缩放的轻点容差后，本次触摸不再
启动应用，即使滑回原位或处于无法继续滚动的页面边界也是如此。在翻页动画中按下
同样不会启动图标；正常轻点允许小幅抖动，静止长按仍可整理图标。

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
