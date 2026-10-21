# DOIT GameKit：NES 0.1.23 实机验证

2026-10-20，完成原先等待原生 PCM 的设备验证待办。目标是旧款 **DOIT GameKit**，
序列号 `1c:29:04:d0:af:5e`，本次端口 `/dev/ttyACM0`、2000000 baud；
不是另一块 `/dev/ttyACM1` 的 GameKit-1。二者结果不应混用。

## 先决条件与版本

先只读检查 PCM 任务 `01a14cb0-1fb2-7d03-936f-4f2f43000d85`，
确认原生 PCM 报告 `build/game-native-pcm-20261020/probe-device-final/report.json`
为 `qualified=true`；最新音频任务状态 completed/idle，明确报告退出桌面。
其最终状态记录显示所有应用 inactive，无相关设备测试进程，才开始本轮设备操作。
不是仅根据串口暂时空闲推断。

使用预先构建并离线验证的 **NES 0.1.23 / release 26** 完整 S31 AOT 包，
包和 AOT SHA256 均与准备记录一致。设备原版本为 0.1.22；更新保持原签名身份。
没有刷写固件，运行固件为前一任务选择的音频 CPU0 版本，ELF：
`d28a4cde0e1e1a2c9de25bb44e07568e03c5434889a1cda13b26a01a8c5867cf`。

本轮是 0.1.23 的独立验证，不是后续 0.1.27 动态跳帧版本的对照实验。
完整标识、先决条件证据和汇总见
[summary.json](nes-gamekit-ppu-20261020/summary.json)。

## 性能

声音开启，马里奥为持续右键与 A 的运动测试；重装机兵为开场房间对话场景。
各采集三个新鲜的约 5 秒窗口，校验运行版本、设备时间和 Mapper。

| 游戏 | 模拟 fps | LCD 完成 fps | 三窗口积压帧 | 模拟速度验收 |
| --- | ---: | ---: | --- | --- |
| 马里奥 PAL | 45.68 | 3.66 | 115 → 128 → 136 | 未通过 |
| 重装机兵 NTSC | 60.04 | 11.05 | 0 → 1 → 1 | 通过 |

马里奥窗口为 41.35、47.31、48.39fps；PAL 目标为 50Hz，积压增长，未达标。
重装机兵为 59.92、60.07、60.11fps，目标为 60Hz，积压 0–1 帧，本场景通过。
这不能推广为所有场景、所有 Mapper 都满速。

LCD 完成 fps 用 `PERF START display` / `PERF STOP` 的
`completed * 1e6 / window_us` 计算。该窗口包含预热，记录数组满后完成计数仍继续。
它不是 Guest 提交次数，也不是 LCD 内部扫描频率。重装机兵日志里的提交速率
约 11.5fps，完整显示测量窗口的完成速率为 **11.05fps**。
截图、存储读取与内存查询均放在性能窗口外。

原始记录：[马里奥](nes-gamekit-ppu-20261020/mario/results.json)、
[重装机兵](nes-gamekit-ppu-20261020/metal/results.json)。

## 色彩、输入、音频

- 重装机兵标题的完成输出缓冲以无损 RGB565 捕获，与 FCEUX 同调色板参考
  **67,840/67,840 像素完全一致**，非调色板像素为 0；排除顶部 20 行。
  [结果](nes-gamekit-ppu-20261020/colors.json) /
  [截图](nes-gamekit-ppu-20261020/metal-title.png)。
  此版本按 320×240 全宽显示，参考图使用相同最近邻映射。
- 游戏输入使用标准 controller-state 注入；日志 `keys_auto=1, keys_alpha=0`，
  运动场景和截图确认按键隐藏。不是机械按键按压验收。
  该板无实体触摸；仅系统解锁和音频授权使用诊断指针，坐标来自本板 320×240，
  没有使用 PAI Touch 坐标或把注入当作实体触摸。
- 马里奥测试前后及最终暂停菜单均显示声音开启，Host 音频资源为 40,560 字节；
  六个正式性能窗口的 `audio_drop` 均为 0。
  0.1.23 没有后续版本的周期 `NES SCHED sound` 字段，因此采集器里的
  `audio_enabled=false` 表示无法由该字段确认，不表示菜单已关闭声音；
  声音状态依据菜单截图及资源快照。没有新增扬声器听音或录音验收，
  低于目标模拟速度时也不能仅凭无队列丢样证明音频实时连续。
- 最后 `INPUT CANCEL` 返回 `status=processed`，解除注入并恢复 GPIO 上报；
  [最终证据](nes-gamekit-ppu-20261020/final-paused.json)。

## 数据保留与内存

更新前将原 NES 私有文件完整备份到本地。更新后、测试后重新读取并比较：
原有 `nes-5760d068.sav`（8192 字节）的 SHA256 始终为
`70a02eeef978c134f8b3e00818271366761dd7e03ad3b74c5926873b5d77b276`。
本测试 ROM 另生成 `nes-6f2a4afc.sav`，没有覆盖原文件。
Duke 0.1.35、OpenLara 0.5.2 的身份、版本与安装状态保持一致。

最终 NES 暂停、声音会话开启时：

| 项目 | 字节 |
| --- | ---: |
| SRAM 空闲 / 最大连续块 | 123,207 / 98,304 |
| PSRAM 空闲 / 最大连续块 | 10,708,364 / 9,437,184 |
| Host 资源预算 internal / external | 26,488 / 49,216 |
| 其中音频 external | 40,560 |
| Host 分配失败 / 拒绝计数 | 0 / 0 |

这是采样时快照；全局最低水位包含此前其他应用运行，不能归因于本轮。
Host 资源预算与整机堆不是同一统计范围，Surface 映射也不等于没有帧缓冲。
对照见 [更新前](nes-gamekit-ppu-20261020/before-snapshot.json) /
[更新后](nes-gamekit-ppu-20261020/after-install-snapshot.json) /
[测试后](nes-gamekit-ppu-20261020/after-tests-snapshot.json)。

设备最后保留在重装机兵的暂停菜单，声音开启，按手柄 X 继续。
可运行包保存在 `local/nes-gamekit-ppu-20261020/package/pxa-nes.pxa`；
用户 ROM 和存档内容只保存在本地，没有复制到公开报告目录。

## 复测

先进入实际游戏，确认声音开启；用设备序列号防止端口变化导致测错板。

```sh
python3 local/pxa-apps/nes/tests/device_perf.py \
  --port /dev/serial/by-id/usb-1a86_USB_Single_Serial_5CFF043509-if00 \
  --baud 2000000 --board doit-gamekit --serial 1c:29:04:d0:af:5e \
  --input-mode controller --hz 50 --mapper 0 --motion mario \
  --warmup 12 --samples 3 --jpeg --output local/gamekit-nes-retest-mario
```

重装机兵改为 `--hz 60 --mapper 74 --motion none --warmup 20`。
0.1.23 不使用要求周期声音字段的 `--require-audio`；单独保留菜单及音频资源证据。
