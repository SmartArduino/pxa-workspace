# PXA 固件 0.11.5

本版改善 ESPP4-A 原生桌面、状态栏和系统控件的大面积实色绘制。
配套版本为 System / DevKit 0.11.2、Host SDK 0.11.2+idf.1、Agent Kit 0.7.5
和 Browser Host 0.11.3。现有应用继续使用 Guest Core 2.1、Host C ABI 2 与现有 AOT ABI。

## ESPP4-A 原生绘制

PSRAM 中足够大的不透明 RGB565 区域默认使用 PPA 硬件填充。
提交给 DMA 的整行块同时满足起止缓存行对齐，首尾不足的行仍由 CPU 填充，
避免缓存同步触及相邻控件的像素。硬件调用返回错误后，本次启动使用原有路径。
小区域、遮罩与半透明绘制继续使用现有实现。

构建选项 `PXA_ESPP4_LVGL_PPA_FILL` 默认开启，可设为 `OFF` 使用原有填充路径。
开发选项 `PXA_ESPP4_LVGL_PPA_SELFTEST` 默认关闭；开启后，在首次硬件填充前
检查颜色、对齐和相邻保护区，并输出诊断计时。
详见 [ESPP4-A 接入说明](../firmware/boards/espp4-a/README.md)。

## 升级

已有设备升级前应匹配板型、芯片和实际分区表，使用 app-only 镜像保留应用、
存档、字体与设置。首次安装使用包含预装字体和商城的完整镜像。
自适配板型应根据对应硬件重新构建；原生 USB 与 UART-DEBUG 保持各自的连接配置。

## English

Firmware 0.11.5 accelerates large opaque ESPP4-A LVGL fills with PPA. DMA spans
stay inside complete cache lines; CPU edge handling and error fallback preserve
adjacent pixels. The feature has a build switch and an optional hardware self-test.
Existing Guest Core 2.1 applications and DevKit 0.11.2 remain compatible. Match
the board and partition layout before an app-only update that preserves user data.
