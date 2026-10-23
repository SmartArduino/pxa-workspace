# PXA 0.5.0

本版更新 SDK、开发工具、桌面模拟器和 ESP32-S3 / ESP32-S31 固件。应用独立发行；工厂数据镜像内置商城 0.9.0，支持短码网页登录和跨端点赞。

## 主要变化

- Core 2.1 增加可选原生主循环事件端口；保留 Core 2.0 应用的加载检查，按声明的服务版本和 feature bits 判断兼容。
- 扩展 C / C++ SDK 与 Host 音频合成、PCM 消费端转换、GameRender 索引绘制和优先级绘制接口，减少高频跨边界调用及重复数据处理。
- 完善多卷安装、签名资源规划、私有目录与 SD 媒体访问；支持动态 UTF-8 文件名和游标保持的随机读取，供大型 ROM 分页。
- 改善 GameKit 软件旋转、LCD 提交及 LVGL 绘制；修复手柄桌面翻页、导航与组合键、唤醒和解锁的交互。
- 桌面模拟器支持原生主循环应用及延后 Surface 释放；保留各应用的独立存档和签名身份。

## 下载与兼容

- [SDK、工具链、DevKit、模拟器与容器](https://github.com/SmartArduino/pxa-system/releases/tag/v0.5.0)。Linux x86_64 完整 DevKit 要求 glibc 2.39+。
- [板级固件](https://github.com/SmartArduino/pxa-workspace/releases/tag/v0.5.0)：PAI Touch、ESP Mosaico、Korvo、SenseCAP Watcher、DOIT GameKit、DOIT GameKit 1。
- AOT ABI 仍为 `wamr-pxa-aot-v6-core-1`，WAMR、LLVM 和 WASI 工具链输入保持固定。新增服务功能需在应用包中显式声明；旧 Host 不会因为 DevKit 发行号相同而自动获得新能力。
- Windows / macOS 使用容器构建与宿主机串口工具；本版不提供未经验证的原生完整 DevKit。

## 固件升级

`merged.bin` 为首次部署镜像，包含分区表、系统字体和商城，会覆盖应用、存档及设置。保留数据的升级应核对原分区表与对应板型的 `flash-map.json`，只刷写 `app.bin`；已有商城可单独更新。不得跨板型混刷。

ESP Mosaico 的应用分区从 4 MiB 扩至 5 MiB，数据分区起点相应移至 `0x510000`。旧版 4 MiB 布局不能直接刷写本版 `app.bin`，否则会覆盖旧数据分区；应先导出应用私有数据、存档及设置，再使用完整镜像迁移，之后按原签名身份恢复数据。本版不在设备上自动迁移或烧录。

本版发布验收不包含重新烧录实机或重新测量游戏帧率；构建与模拟器结果不等同于真机 FPS 承诺。EPDiy 接入代码随源码提供，未列入本版预编译固件。
