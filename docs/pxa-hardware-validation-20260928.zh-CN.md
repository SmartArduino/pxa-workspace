# PXA 实机验证：2026-09-28

本轮在用户提供的两块设备上构建、刷写并验证资源和游戏路径，修复实际发现的问题。完整资源重构目标仍未完成；本文区分已执行的硬件测试、构建验证和待完成的性能验收。

## 设备与刷写范围

| 设备 | 实际芯片 | 连接 | PSRAM |
| --- | --- | --- | ---: |
| Pai Touch | ESP32-S3 rev 0.2，Xtensa，240 MHz | `/dev/ttyACM0` | 8 MiB |
| ESP32S31 Korvo-1 | ESP32-S31 rev 0.0，RISC-V，300 MHz | `/dev/ttyUSB0`，2000000 baud | 16 MiB |

刷写前分别备份完整 16 MiB Flash，并确认设备分区表与构建一致。只刷写 `0x10000` 的应用分区，没有擦除 bootloader、分区表或 NVS；应用安装和权限设置会修改数据分区。原有用户应用保留，最终查询两设备均正常响应，应用均为 inactive。

证据根目录为 `build/resource-refactor/hardware-20260928/`。原始备份为 `pai-original-flash.bin`、`korvo-original-flash.bin`，分区及备份身份见 `original-flash.json`。源码、最终固件、构建日志及分阶段测试结果汇总于 `report.json`，由同目录 `finalize.py` 生成。报告中的 SHA 用于构建产物和测试证据识别，不是运行时资源校验；没有恢复运行时 SHA 或安装后修改检测。

## 实际修复

1. **首次启动私有存储目录缺失。** KV 初始化原来依赖 FS 服务先创建父目录；只使用 KV 的新应用可能报 `create-private-storage-root`。现在 FS 和 KV 共同使用创建私有根目录的辅助函数，KV 可以独立初始化。Host 回归从父目录不存在的状态开始，覆盖创建、保存与重载。
2. **系统状态刷新重建整个页面。** 电量、时间等更新会重建 UI，导致权限弹窗关闭、页面状态丢失。现在这些更新仅刷新可见状态栏及锁屏内容；复用状态栏父对象，避免重复登记回调。真实 Pai 上观察 22 秒，状态栏以下画面逐像素一致，权限开关可完成操作。涉及音量、亮度等控制字段或通知面板展开时仍会走原有重建路径，尚未实现所有状态的增量更新。
3. **展开后的调色板错误使用内部 SRAM。** voxel 的光照调色板为 8192 B，失败时内部 SRAM 最大连续块只有 7680 B，虽然仍有 PSRAM。默认策略统一为不超过 512 B 的调色板使用内部内存，大调色板进入外部内存。Core、POSIX worker 和 ESP worker 共用同一策略；测试覆盖大调色板预取、加载、预算和回收。修改后实际 voxel 进入菜单与游戏。
4. **Korvo IMAGE 像素缓冲区对齐不满足 LVGL。** S31 的 `LV_DRAW_BUF_ALIGN=64`，原始对象尾随数据不能保证此对齐，plane-shooter 的 Canvas PRESENT 返回 `-3`。IMAGE 现在在单次分配中预留 63 B，并将像素起始地址对齐到 64 B，全部计入预算，没有新增整图复制。纹理、调色板、音频对象不增加此项开销。测试使用不同合法分配基址验证对齐、像素与释放。

Korvo 同时接入共用 `PxaAudioOutput`，沿用其有界输出和生命周期处理；物理输出仍由板级 codec 完成。构建审计新增 S31 对应 codec 档案，S31 检查 84 个对象，S3 检查 85 个对象，均检查四个强预算入口。Guest UI 的普通命令发送增加可选失败诊断钩子，plane-shooter 用它定位了上述 PRESENT 错误；成功路径不输出日志。

## 文件资源、音乐与循环退出

`tools/verify-device-resources.py` 在启动前订阅日志，验证 8193 B 文件读取和 EOF、音乐 READY、100 场景/20 张文件纹理/4 张可见、100 次准备后音效触发及匹配的音乐 STOPPED，随后停止应用并确认 inactive。它保存串口事件、设备信息及每轮结果；这些完成标记不等于每个场景的像素正确性证明。

| 设备与固件阶段 | 三轮完成时间（秒） | 各轮退出后的 free_heap（B） |
| --- | --- | --- |
| Pai，最终固件 | 23.596 / 23.561 / 23.603 | 7276911 / 7276927 / 7277011 |
| Korvo，存储/状态/音频修复后 | 28.917 / 28.947 / 30.464 | 12114755 / 12114815 / 12114783 |

两设备各三轮均完成，音乐日志的 underruns 和 missing 均为 0。未使用麦克风测量实际声学输出；总 free_heap 接近不等于完整原生分配核算或无泄漏证明。完成时间包含应用节奏、I/O、解码及串口观测，不是帧时间。

实际构建的外部资源缓存为 512 KiB，共享外部资源预算为 2 MiB，两者不同。Korvo 此轮记录缓存外部峰值 459104 B、内部峰值 536 B、metadata 30039 B、任务栈 6144 B、驱逐 393 次、失败 0 次。Pai 最终轮在退出附近存在日志丢弃，没有捕获对应峰值，不能用 Korvo 数字替代。

**阶段边界：** Korvo 三轮资源测试早于最后的大调色板和 IMAGE 对齐修改，目录为 `korvo-final/`；最后的 S31 固件由后述 plane-shooter 验证。Pai 三轮则使用最终固件，目录为 `pai-final-firmware/`。不能把 Korvo 早期循环标注成最终 ELF 的执行结果。

截图 `pai-capture/round-1.png` 和 `korvo-visual/textures-3.jpg` 可见四张纹理。部分其他截图恰好落在应用刻意保留的黑色加载帧，原始截图一并保留。截图传输扰动运行时间，未用于 FPS 统计。

## 实际游戏

### Pai：voxel-craft

新资源版本已安装，并在调色板修复后进入真实地形游戏，验证触摸转向、约 17 秒后台后回到前台及退出。记录在 `pai-voxel-palette/`，包含 `menu.jpg`、`gameplay.jpg` 和 `turned.jpg`。本次运行早于最终 IMAGE 对齐修改，voxel 的纹理/调色板不使用该 IMAGE 填充路径。

296×240 场景观察约 **16 FPS**；最后累计 submitted/rendered/visible 为 3241/3240/3239，dropped 为 1。单帧 raster 样本约 24–41 ms，不能当作 p50/p95。此次没有同画质旧版实机基线，也没有证明性能提升。后续应分别测量逻辑、光栅化、合成和显示等待，再决定优化点。此轮音乐 underruns/missing 均为 0。

### Korvo：plane-shooter

最终对齐修复固件已运行文件图片版本，实际显示菜单并进入战斗，证据位于 `korvo-plane-aligned/` 的 `home.jpg`、`battle.jpg` 和 `battle-resumed.jpg`。等待操作期间飞机已被击毁；之后后台约 21 秒并恢复，保留游戏结束画面。这验证了应用后台/前台路径与该状态的保持，不能冒充活动战斗暂停计时验收，也没有可靠帧率分位数。

首次安装遇到 LittleFS 空间不足，仅卸载本轮新增的 resource-scenes 后重试成功，没有移除原有用户应用。Korvo 最终保留新 plane-shooter，resource-scenes 已移除。大屏 UI 仍需完整布局验收。

## 构建与回归

- 最终完整资源回归 `alignment-checks.log` 通过，36 份日志保存于 `final-regression-logs/`，包含 31 项 Core/WAMR、ESP Host、规范/包工具、真实 LVGL sanitizer 及 15 组 AOT 场景。
- Pai、Korvo 和 Sensecap 最终固件依次构建通过，并通过 codec 预算入口审计；Pai/Korvo 实际刷写验证成功。Sensecap 本轮只有构建证据，没有硬件运行证据。
- Pai 与 Sensecap 的 desktop/product 模拟器入口最终均重建通过，见 `sim-pai-final.log`、`sim-sensecap-final.log`。早期状态修复阶段 CTest 为 13/13；这不是最终代码重新执行 CTest 的记录。
- 对齐边界、展开调色板和首次 KV 创建目录均有针对性回归，工具通过 Python 语法检查。

资源循环复现命令如下，前提是已经安装对应架构的 resource-scenes 包并完成权限设置（Korvo 当前为了空间已卸载此测试应用）：

```sh
python3 tools/verify-device-resources.py --port /dev/ttyACM0 \
  --rounds 3 --output build/resource-refactor/pai-repeat
python3 tools/verify-device-resources.py --port /dev/ttyUSB0 --baud 2000000 \
  --rounds 3 --output build/resource-refactor/korvo-repeat
```

## 剩余验收

完整目标仍包括普通 UI 属性/MOVE/网格准备失败的事务回滚、旧 UI 图片路径迁移与删除、LVGL/alpha/线程/帧缓冲等完整原生内存核算和准入，以及异常与生命周期矩阵。本轮没有实际熄屏/长期睡眠唤醒/断电测试。voxel 和完整游戏工作负载还需要同画质、多轮 CPU 与整帧 p50/p95 基线比较，不能以资源循环耗时或截图 HUD 替代。
