# PXA 固件工作区 0.11.1

状态：发布准备中。系统与工具变化见
[PXA 0.11.1 与 Agent Kit 0.7.3](../deps/pxa-system/docs/release-0.11.1.zh-CN.md)。

Pai Touch 使用优先级 4 的摄像头工作任务，使 RGB/JPEG 工作及时完成；共享板级
实现仍默认优先级 2，各板可设置 `PXA_BOARD_VISION_TASK_PRIORITY`。
保留 GC2145 单帧缓冲、原有分辨率和颜色转换路径。

ESPP4-A 使用 LVGL 原生 DIRECT 显示模式并减少桌面与状态栏重复合成。
PXADB 支持原生 USB 和 UART-DEBUG 双通道，UART 默认 2,000,000 波特率；
连接较差时可选择 1,000,000。CH340X 链路使用其支持的速率范围，避免默认 3,000,000。

固件维护 pai-touch、esp-mosaico、esp32s31-korvo-1、sensecap-watcher、
doit-gamekit-2、doit-esp32s31-devkit 和 espp4-a 七种板型，每份完整镜像包含字体与商城。
原生 USB 与 UART 的 PXADB 解析和会话互相独立；网页从设备选择悬浮面板发起安装。
USB 转串口设备在打开端口并握手后显示协议报告的设备名称，支持自适配板。

完整镜像会覆盖数据分区。保留数据升级前须比对分区表，只写入匹配的应用分区。
Mosaico 与 Korvo 的旧布局迁移说明见发行包 README；不要将更大的应用镜像直接
写入旧版较小分区。应用包、固件和商城能力快照分别编号。

## English

Preparation only. Pai Touch raises its camera worker priority to 4 while other boards
retain the shared default of 2. The GC2145 resolution, color conversion and single
frame buffer are unchanged. ESPP4-A uses native LVGL DIRECT presentation and avoids
redundant launcher/status overlay composition. PXADB supports both native USB and
UART-DEBUG; UART defaults to 2 Mbps with a 1 Mbps fallback for suitable bridges.

Seven maintained board images include the system font and Store. USB and UART have
independent protocol sessions. The web installation panel identifies authorized ports
through a PXADB handshake, including custom boards behind USB serial bridges.
Merged images replace data. Compare partition maps before an application-only update,
and follow the documented Mosaico/Korvo migration procedure for older layouts.
