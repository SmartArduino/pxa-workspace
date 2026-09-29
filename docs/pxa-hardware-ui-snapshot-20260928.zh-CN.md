# 最新 UI 固件实机验证：2026-09-28

本轮验证包含网格/MOVE、普通属性失败回滚和透明层 snapshot 缓冲复用的固件。它晚于[前一轮硬件验证](pxa-hardware-validation-20260928.zh-CN.md)，两轮证据分别保存。完整资源重构目标仍未完成。

## 刷写与设备现状

Pai Touch 使用 `/dev/ttyACM0`，Korvo-1 使用 `/dev/ttyUSB0`、2000000 baud。重新备份两块设备各 16 MiB Flash，确认分区表与构建一致后，仅刷写 `0x10000` 应用分区，esptool 写入验证通过。没有刷写分区表、擦除 NVS 或删除原有应用。当前安装列表与上一轮不同，以本轮实际设备状态为准。

证据位于 `build/resource-refactor/ui-snapshot-hardware/`。`report.json` 由同目录 `finalize.py` 汇总备份、固件、日志及截图身份。刷入固件是 `ui-snapshot-final-firmware/{pai,korvo}/pxa_esp_platform.bin`，分别为 3264256 B 和 3590624 B；源码、编译参数与目标布局另见 `ui-snapshot-report.json`。Sensecap 此阶段只有构建结果，没有实机执行结果。

## Pai：三轮资源、音乐与音效

新安装 resource-scenes，并通过设备设置授予必需的 `audio.playback` 权限。授权前的启动明确被权限策略拒绝，该次没有进入资源测试。随后执行：

```sh
python3 tools/verify-device-resources.py --port /dev/ttyACM0 --rounds 3 \
  --output build/resource-refactor/ui-snapshot-hardware/pai-resources
```

每轮都完成 8193 B 分块读取及 EOF、音乐 READY、20 张文件纹理中四张可见的 100 次场景切换、100 次准备后音效触发及匹配的音乐 STOPPED，退出后确认 inactive。

| 轮次 | 完成时间（秒） | 退出后 free_heap（B） |
| --- | ---: | ---: |
| 1 | 23.565 | 7231575 |
| 2 | 23.465 | 7231307 |
| 3 | 23.467 | 7231659 |

三次音乐计数均为 underruns=0、missing=0。串口报告丢弃了 13 条日志，但所有完成标记和三次音乐计数均已捕获；没有完整缓存峰值，不能用缺失日志证明不存在异常。完成时间包含场景节奏，不是帧时间；聚合 free_heap 接近也不是完整原生堆无泄漏证明。设备保留了这次新增的资源测试应用，测试结束时 inactive。

## Korvo：现有两个游戏

直接运行设备上已有的 plane-shooter 和 voxel-craft，未替换它们的 Guest 包；其实际安装内容保存在本轮刷写前的完整 Flash 备份中，不能默认等于当前工作区重新构建的包。

- `korvo-plane-timed/`：显示文件图片菜单，进入活动战斗，截图可见存活飞机；后台约 15.031 秒后恢复，飞机仍存活，随后退出。共享资源计数记录外部峰值 203399 B、metadata 30547 B、任务栈 6144 B、失败 0。两次 plane 流程结束后的 free_heap 均为 12083171 B。
- `korvo-voxel/`：显示菜单和 800×480 实际地形，后台约 15.035 秒后恢复，场景及操作控件可见，退出回到 inactive。音乐 underruns/missing 均为 0；外部资源峰值 47052 B、metadata 31223 B、任务栈 6144 B、失败 0。启动前/退出后 free_heap 为 12077423/12077379 B。此运行丢弃 11 条串口日志。

这些资源数字不包括完整 Guest、帧缓冲、LVGL 和原生分配。截图传输会扰动运行；日志中的 EMA、Guest FPS、HUD 和单帧样本不作为整帧 p50/p95 或性能改善证据。后台恢复的功能检查也不等于完整暂停计时与睡眠矩阵。

最初 `korvo-plane/` 的恢复截图紧接异步启动请求，拍到的是系统桌面，不能据此判断战斗画面。后续 `korvo-plane-timed/` 增加等待，保存了实际战斗及恢复截图；两次原始记录均保留。订阅串口日志会先排出上一轮缓存，分析时按本轮启动及设备时间戳划定边界。

## Korvo 安装失败与清理

本轮尝试安装 resource-scenes 返回 `prepare-incoming status=-11`。带日志重试明确捕获 LittleFS 的 `No more free space`，因此没有完成该固件上的 Korvo 资源循环。失败后安装列表没有发布半成品 resource-scenes；仅清理本轮上传的 `/pxa-state/inbox/pxa-resource-scenes.pxa`。原有已安装应用和已暂存包均保留，没有通过删除它们来凑空间。

## 验证边界

本轮最终两设备均响应，应用均 inactive。固件配套的完整主机资源回归、15 项真实应用像素比较、三板顺序构建及两配置模拟器构建均已通过，细节见[执行记录](pxa-resource-refactor-progress.zh-CN.md)。暖态 snapshot 创建次数 100→0 是主机适配器回归的结论，不冒充硬件原生分配观测。

完整原生内存核算、原生 OOM 事务性、剩余旧路径迁移、受控同画质整帧性能比较、实际熄屏/睡眠唤醒和其他异常矩阵仍待完成。没有进行声学测量。运行时 SHA 和安装后修改检测保持移除；报告中的摘要只用于备份与构建证据识别。
