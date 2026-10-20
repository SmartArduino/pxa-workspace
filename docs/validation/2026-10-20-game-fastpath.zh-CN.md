# 原生主循环的 GameRender 画面转换快速路径

本次复用 GameRender 1.2 的 `borrowed-indexed-blit`，为原采样率 PCM、
Core 2.1 主循环增加两个可复现构建配置：

- `duke-native-blit`：Duke 0.1.35 / sequence 36。
- `quake-native-lit-blit`：Quake 0.4.14 / sequence 134，保留已有精确光照缓存。

两者使用共享适配层的 `PXA_RECROF_HOST_BLIT`，不修改上游引擎或增加另一套 ABI。
普通 Apps 配置仍保留原来的默认值；上述签名配置显式声明所需服务和执行模型。
不能将旧的 callback `duke-blit` 数据混作本次主循环版本的对照。

## 快速路径及内存

每帧提交 64 字节描述，包含已有索引画面、512 字节调色板和已取得的
RGB565 Surface 的地址及范围。Host 在同步调用期间借用线性内存，
验证边界和读写重叠，再直接写入目标。320 像素宽画面使用两个 RGB565
像素合并存储；缩放和透明光标仍保持原采样规则。

这一步不复制源图到命令包，不增加整帧中间缓冲、不建立大型调色板缓存，
不改变分辨率、纹理、光照、颜色精度或 PCM 配置。Host 拒绝某帧时，
共享运行时仍可执行原 Guest 转换。上下文使用现有 Host 预分配槽位。

相对各自对照包，Duke AOT 增加 4,812 字节，Quake 增加 4,848 字节；
线性内存上限仍为 56 页。新增内容主要为接入及协议代码，
不能把“没有新增帧缓冲”解释为二进制大小完全不变。

## 固定环境与正确性

- GameKit MCU：`1c:29:04:d0:af:5e`。
- 固件 ELF：`c5eef7676c5c387e1c5fe05d8753983137efff9edd3182ee4ce9669e768b6845`，
  本轮没有重新刷写固件。
- SDK：`b787d4a`；Apps：`f480f2c`；使用已有独立 known-alignment AOT 编译器。
- 保留原 SD 资源、存档、画质和音频格式；每次安装前核验资源 SHA-256。
- 每个 FPS 窗口 60 个约一秒间隔的样本，开始前确认实际关卡和 PCM 协商。
  对日志按设备时间排除订阅重放记录，保留窗口内不同的关卡记录。

`run_host_indexed_blit.py` 的 ASan/UBSan 检查通过：500 组原转换函数与 Host
结果逐像素比较，覆盖 320×200/240、任意缩放、光标、奇数字节分区、
越界保护及重叠拒绝。它验证转换正确性，不替代设备性能测量。

完整包、AOT、Wasm、编译器和相关源码哈希在
[artifacts.json](game-fastpath-20261020/artifacts.json)。
原始串口记录无损压缩，另存解压后的 SHA-256；截图来自设备实际完成的帧。

## Duke 实际关卡

E1L1 Hollywood Holocaust，出生点 `(-31243,7160,-158228)`，角度 422，
生命 100。两组均隐藏触控按钮，保持原画质和 11025 Hz 双声道源 PCM。

| 构建 | 平均 FPS | 中位 FPS | 最低 / 最高 |
| --- | ---: | ---: | ---: |
| 0.1.34，Guest 转换，上一轮对照 | 18.383 | 18.35 | 17.8 / 18.9 |
| 0.1.35，Host 转换，本轮 | 19.763 | 19.9 | 18.8 / 20.9 |

平均提升约 **7.5%**，尚未达到持续 ≥20 FPS，更不代表移动或战斗均保持此速度。
`draw_us / frames` 从 12.92 ms 降至 **10.24 ms**，减少约 **20.8%**；
该计时包含取得缓冲、转换、控件绘制和提交，不能全部算作转换耗时。
Host 单次执行约 **2.55 ms**，窗口内 batch 从 1,317 增至 2,412，
拒绝计数保持 0，证明没有静默退回 Guest 路径。

10 条不同的实际关卡记录覆盖 55.234 秒。采样完成后验证：

- 手柄隐藏、松开保持隐藏、首次触屏恢复、断开恢复全部通过。
- 首次触屏前后 HUD 区域 `(0,200)-(260,240)` 逐像素一致，无误开火。
- 前进后位置 `(-30498,12176,-158422)`，后退后位置
  `(-32104,7123,-158386)`，不是仅依据输入命令的 ACK 判断移动成功。
- 正常释放输入、停止应用、返回桌面；本次脚本无错误退出。

内存快照为 SRAM 空闲 104,027 字节、PSRAM 空闲 9,688,568 字节。
Host 预算内 internal 为 26,488 字节，临时 internal 峰值为 0，
分配失败为 0；整机空闲值受运行历史影响，不作严格的增量内存对照。

[完整测量](game-fastpath-20261020/duke-blit-device/measurement.json) /
[核对结果](game-fastpath-20261020/duke-blit-device/audit.json)。

## Quake 光照缓存对照

先测 0.4.13：原精确光照缓存、Guest 转换和隐藏触控按钮，START 出生点
`(544,288,50)`、生命 100。60 秒平均 **8.75 FPS**，中位 **8.9 FPS**，
最低 7.9、最高 8.9。11 条不同关卡记录覆盖 50.501 秒。
视图耗时中位 88.34 ms，表面耗时 54.04 ms，画面转换及提交 12.21 ms。
手柄／触屏／断开切换全部通过，正常停止。

此前 0.4.11 的平均 8.367 FPS、表面耗时 59.18 ms 是历史参考；
那一版同时没有本次按钮隐藏行为，不能把全部帧率变化只归因于光照缓存。
本轮 0.4.13 与 0.4.14 才是原生画面转换的直接对照。

[0.4.13 测量](game-fastpath-20261020/quake-lit-device/measurement.json) /
[核对结果](game-fastpath-20261020/quake-lit-device/audit.json)。

## Quake 原生画面转换结果

0.4.14 相同出生点、相同画质的 60 秒结果：

| 构建 | 平均 FPS | 中位 FPS | 最低 / 最高 | 画面转换及提交 |
| --- | ---: | ---: | ---: | ---: |
| 0.4.13，Guest 转换 | 8.75 | 8.9 | 7.9 / 8.9 | 12.21 ms |
| 0.4.14，Host 转换 | 8.85 | 8.9 | 7.9 / 9.9 | 9.78 ms |

输出阶段减少约 **19.9%**，但总帧率均值只增加 0.10 FPS，中位数没有变化，
不能据此宣称 Quake 整体获得明显提速。新窗口视图耗时中位 89.34 ms，
其中表面耗时 55.28 ms，仍是核心瓶颈；当前远未达到 15/20 FPS。
这条快路加速的是最终画面转换，不能替代对纹理、光照内循环的优化。

12 条不同的关卡记录覆盖 55.706 秒。Quake 的 `hb/hr` 是每个统计周期的增量，
不是 Duke 的累计计数：这些记录合计 542 次原生执行，拒绝 0 次，每条记录
均满足 `hb == n`；Host 执行中位每帧 2.273 ms，校验 0.073 ms。

手柄隐藏、松开保持隐藏、触屏恢复及断开恢复均通过，正常释放输入、停止应用、
返回桌面。SRAM 空闲 91,755 字节、PSRAM 空闲 9,147,820 字节；
Host 预算内 internal 仍为 26,488 字节，临时 internal 峰值及分配失败均为 0。
这里没有把不同运行历史下整机空闲内存的差值当成精确的分配增量。

[0.4.14 测量](game-fastpath-20261020/quake-lit-blit-device/measurement.json) /
[核对结果](game-fastpath-20261020/quake-lit-blit-device/audit.json)。

测试安装为腾出内部应用代码空间轮换归档了本线程游戏，资源与存档保留；
未归档 NES 或 OpenLara。测试结束时保留安装 Quake 0.4.14，并已返回桌面。
Duke 快速路径签名包仍在本地构建输出目录。

## 复现构建

从根目录运行，已存在 SDK、游戏源码及独立编译器时：

```sh
PXA_GAME_APPS_ROOT="$PWD/local/pxa-apps" \
PXA_BUILD_CACHE_DIR="$PWD/build/game-fastpath-20261020/wasm-cache" \
WAMRC="$PWD/build/game-native-pcm-20261020/wamrc-known-alignment/wamrc" \
bash tools/app.sh build duke-native-blit --board doit-gamekit \
  --source-root tools/game-perf/profiles --target esp32s31 --aot-only \
  --output build/game-fastpath-20261020/packages
```

Quake 使用 `quake-native-lit-blit`。对照时还需固定源码、资源、场景、
固件和编译器，不能只比较配置名称。上游更新继续使用已有
`common/recrof/tools/upstream.py` 工作流，随后重新运行差分测试和设备测试。
Quake II、C&C 和 Red Alert 尚未通过这条主循环快速路径的本轮设备验收。
