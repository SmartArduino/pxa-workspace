# 完整主引擎验证证据

此目录记录 TR1 完整主引擎移植的验证，资源只有公开 PC 演示版的标题和 City。全关卡验收需要原版数据；这些结果不代表 15 关已经通关，也不提供完整商业资源。

`memory-probe.cpp` 直接运行上游主引擎，固定 320×240、90 次 34 ms 更新；`memory-reference.log`、`memory-optimized.log` 与 `memory-proof.json` 比较原始数组复制和借用数组/转移网格所有权。两组保留相同编译优化级别，关卡像素 SHA-256 完全一致。此探针不是 Core 2 Host，不能将它的内存绝对值当作应用总内存或真机 PSRAM。完整 Core 2 Guest 峰值另见 integration 记录。

复现探针时使用工作区 WASI SDK 的 clang/clang++，C++11、O1、`-D__BITTBOY__=1 -fwrapv -fno-strict-aliasing -fno-exceptions -fno-rtti`，include 指向应用 `vendor/classic`。先编译 `libs/tinf/tinflate.c`、`libs/stb_vorbis/stb_vorbis.c`、`libs/minimp3/minimp3.cpp` 并链接；WASM stack=65536，max-memory=16777216。第二组仅增加 `-DOL_CLASSIC_MEMORY=1`。用 `run-memory-proof.mjs WASM OUTPUT.rgb565` 执行，两组分别输出到 `local/openlara-full/`，读取默认准备目录中的原始 `level2.phd`。不要将独立 SDK 探针包安装到真机。

应用验证脚本位于 `local/pxa-apps/openlara/tools/`，README 列出执行命令。设备安装失败记录保留了设备身份、固件散列和包列表；当次因闪存空间不足停止，没有测得完整引擎的真机 FPS。另一个应用的备份记录只含代码散列，未归档其代码或清除其私有数据。
