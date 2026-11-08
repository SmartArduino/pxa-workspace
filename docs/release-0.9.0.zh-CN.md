# PXA 0.9.0

## 中文

设备固件、C/C++ SDK、AI Agent Kit 0.5.0、编译器、打包工具与模拟器配套更新。增加 DOIT ESP32-S31 DevKit、ESPP4-A 目标及 Windows x64 原生构建和预编译打包入口。

修复动态加载 PXADB 时的本地 socket 依赖、未初始化输入诊断、Windows WinSock 头顺序及平台清单选择。改善文件异步写入、应用启动与内存映射、板级画面合成和传感器接入。应用兼容性仍由 Core、服务与 AOT ABI 决定，发行号不替代能力检查。

AI 工具新增 device-profile / pxa_device_profile，读取设备公开的分辨率、CPU、调试输入和传感器；设备未连或 PXADB 未开启时给出操作指引。提示词会确认物理操作与自适应屏幕要求，提供 Z Labs 中文像素字体、整数缩放、图块复用与异步存档建议。商城流程提供缺项清单、图标与可选封面生成建议和真实模拟器截图要求。网站、上架接口与教程同步更新。

下载后先验证签名清单与摘要，再解压 DevKit，执行 pxa init hello、pxa run hello --offline。Windows 预编译包标注 Wine 验证；原生 Windows 窗口、音频和 COM 驱动需在目标电脑确认。固件应用镜像仅用于匹配的分区布局；完整镜像会清除应用、设置和存档，迁移前备份。商城提交仍需浏览器账号授权并等待审核。

## English

Matched device firmware, C/C++ SDKs, Agent Kit 0.5.0, compilers, packaging tools and simulators. Adds DOIT ESP32-S31 DevKit and ESPP4-A targets plus native Windows x64 build and precompiled packaging support.

Fixes PXADB local-socket imports for dynamic consumers, uninitialized input diagnostics, Windows WinSock include ordering and platform manifest selection. Improves asynchronous filesystem writes, app launch/memory mapping, board composition and sensor integration. Core/service/AOT capabilities remain the compatibility authority.

New device-profile / pxa_device_profile reads reported CPU, resolution, debug input and sensors, with USB/PXADB setup guidance. AI instructions confirm physical controls and screen adaptation, explain Z Labs pixel fonts, integer scaling, tile reuse and asynchronous saves. Publishing preflight includes metadata/artwork preparation and original simulator screenshots; website and tutorials are updated.

Verify signed manifests and hashes before extracting. Run pxa init hello and pxa run hello --offline. Windows packages identify Wine validation; native window/audio/COM-driver acceptance requires the target PC. App-only firmware requires a matching partition layout; merged provisioning images erase apps, settings and saves. Back up before migration. Store submission requires browser authorization and remains pending review.
