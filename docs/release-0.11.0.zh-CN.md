# PXA 固件工作区 0.11.0

系统、DevKit 与 Agent Kit 的变化见
[PXA 0.11.0 发布说明](../deps/pxa-system/docs/release-0.11.0.zh-CN.md)。

维护固件覆盖 pai-touch、esp-mosaico、esp32s31-korvo-1、sensecap-watcher、
doit-gamekit-2、doit-esp32s31-devkit 与 espp4-a。旧款 doit-gamekit 与
doit-gamekit-1 保留历史下载，不能套用 GameKit 2 镜像。

ESPP4-A 的官方 PXADB 接口为原生 USB HS；CH340 调试口用于控制台和烧录。
不同产品的串口接线、芯片及屏幕配置见各板 README。相机通过精确作用域权限
和前台所有权使用，关闭后释放硬件资源。P4 相机预算及 DMA 缓冲按32 MiB
PSRAM设备配置；Pai 保留内部 SRAM DMA 路径，不启用未经验证的 PSRAM 直采。

首次安装使用完整镜像，会覆盖数据分区。已有设备先核对发行包的分区表；
布局一致时只刷应用镜像，保留原有应用、存档及设置。应用、商城、SDK 和固件
分别编号，应用安装仍以 Core、服务特性、CPU 和 AOT ABI 的真实兼容性为准。

## English

See the linked system release notes for DevKit and Agent Kit changes. Maintained
images cover seven boards, including GameKit 2; legacy GameKit models retain
their historical downloads and require their own firmware. Official ESPP4-A
PXADB uses native USB HS; its CH340 port provides console/flashing access.

Full images replace device data. For an existing installation, compare the
partition map and use an application-only update when layouts match. App,
Store, SDK and firmware versions are independent; compatibility follows actual
Core, service, CPU and AOT requirements.
