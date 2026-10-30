# PXA 0.6.0

本版更新系统、C/C++ SDK、DevKit、桌面模拟器、六种板级固件和 Agent Kit
0.4.0。应用独立发行；工厂商城更新为 0.9.1，另提供轻量商城 0.1.0。

## 系统与 SDK

- 完善标准 Sensor、Haptics、音频捕获、异步存储、资源预算和取消清理；
  多卷安装和私有目录继续以签名身份隔离。
- SDK 分发包含工作区 `pxa-ext` 扩展、合同、CMake 入口和示例。
  `org.doit.peripherals` 0.3 提供逻辑端口、RGB 和有界视觉流；0.1/0.2
  客户端只看到其协商版本支持的端口。扩展不属于 PXA 标准服务。
- 摄像头示例演示逐项授权、分段读取和原子保存；指示灯示例演示后台释放。
  应用必须处理没有硬件、没有可用模型、权限拒绝和设备忙等情况。
- Agent Kit 上架工具读取服务端媒体规格，支持提供图片、由编程助手生成，
  或明确跳过图标/封面；预检尺寸、格式和大小，并填写分类与标签。
  推荐图标 512×512，封面 1280×960（4:3）。宣传插画与运行截图分别上传。

## 板级功能

- Korvo 增加标准 SDMMC/Storage/FS、ES8389 麦克风、RGB 指示灯和
  OV3660 JPEG 采集；摄像头由独立扩展提供。
- SenseCAP Watcher 增加 SD/Storage/FS、ES7243/ES7243E 麦克风、
  PCF8563 保留 UTC、RGB、Grove I²C 与 Himax JPEG/推理读取。
  SD 与 Himax 共用 SPI2；无卡启动仍保留视觉通路。推理使用已有板内模型，
  不自动刷写模型或 Himax 固件。
- GameKit 1 完善电池与输入接入，增加逻辑状态灯端口。
- Mosaico 完善传感器、振动、模块槽发现和存储接入；实际能力以板上硬件
  和枚举结果为准。PAI Touch 保留标准麦克风和独立语音触发扩展。

接线、保留地址、数据格式和限制见各板 README 及
[扩展接入说明](zh-CN/pxa-extensions.md)。SD 挂载失败不会自动格式化；
Watcher/Korvo 的 SD 当前按启动状态挂载，插拔前请关机。Watcher 无卡启动
不代表已完成该设备的实卡读写验证。

## 下载与升级

- [SDK、DevKit、工具链、模拟器和 AI 工具](https://github.com/SmartArduino/pxa-system/releases/tag/v0.6.0)
- [板级固件与商城包](https://github.com/SmartArduino/pxa-workspace/releases/tag/v0.6.0)

Core 仍为 2.1，AOT ABI 仍为 `wamr-pxa-aot-v6-core-1`。服务功能以包声明、
版本协商和 Host 能力快照判断，不能仅按发行号推断。原生完整 DevKit 适用于
Linux x86_64、glibc 2.39+；Windows/macOS 可使用容器和宿主机串口工具。

`merged.bin` 用于首次部署，会覆盖应用、存档和设置。保留数据升级必须先核对
实际分区表与对应 `flash-map.json`，布局一致才可只刷应用区。Korvo 应用分区
扩至 `0x6f0000` 字节，数据区改从 `0x700000` 开始；旧 4 MiB 应用布局不能
直接刷本版应用，应先导出数据，再完整迁移并按原签名身份恢复。Mosaico 仍需
遵守 0.5.0 的分区迁移说明。禁止跨板型混刷。

发布中的 EPDiy 接入保留在源码，未提供该板的预编译固件。构建和模拟器验证
不构成所有板型、外接模块或游戏帧率的真机承诺。
