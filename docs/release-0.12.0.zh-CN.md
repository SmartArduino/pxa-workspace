# PXA 固件 0.12.0

本版嵌入 PXA System 0.12.0。设备仍使用 Guest Core 2.1 和相同的 AOT ABI。
应用可查询实际提供的视频、显示、网络和存储能力，再选择相应功能。

- 新增 Video 0.3 服务及 Guest C/C++ 接口。支持的格式、输出与预算由 Host 报告。
- 改善多款开发板的 RGB565 输出、旋转、触控和显示提交。
- 改善应用安装、更新和存储卷处理，扩展 PXADB2 包安装与大数据传输。
- 更新桌面、输入法和系统手势，并增加 Canvas 椭圆绘制支持。

已有设备升级前先核对板型和分区表。使用 app-only 镜像保留应用、存档、字体与设置。
完整镜像用于首次安装。开发工具由 System 发行提供，配套 Agent Kit 为 0.7.6。

## English

PXA firmware 0.12.0 includes System 0.12.0 and retains Guest Core 2.1 and the existing AOT ABI.
This release adds the Video 0.3 guest API and improves display output, rotation, touch, install and update handling, and PXADB2 transfers. Query the Host for video formats and limits. Match the board and partition layout before an app-only upgrade.
