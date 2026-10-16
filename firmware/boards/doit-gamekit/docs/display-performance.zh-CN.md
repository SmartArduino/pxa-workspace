# DOIT GameKit 显示性能优化

2026-10-16，ESP32-S31，CPU 320 MHz、PSRAM 250 MHz，320×240 RGB565。
ST7789 始终保持原生 240×320 扫描方向，软件旋转 270°，未接 TE。

## 与 pai-touch 的差异

对照 `pai-touch/src/pai_touch_hardware.cc` 和
`pai-touch/include/parallel_sw_rotation_flush.h`。两板均使用 SPI ST7789。

| 路径 | 原 DOIT | 本次 DOIT | pai-touch 参考做法 |
| --- | --- | --- | --- |
| SPI 时钟 | 40 MHz | 80 MHz，可在 menuconfig 调整 | 80 MHz |
| 原生输出缓冲 | 一块 | 两块，每块 153600 B | 两块 |
| 旋转与送屏 | 串行，DMA 完成才释放 LVGL | 复制完成释放 LVGL，独立任务送屏 | 旋转、提交分开，双核旋转 |
| 全屏 GameRender | 每帧 LVGL 合成并复制到逻辑帧 | 直接从 Surface 旋转到原生输出 | Surface 直通 |
| 游戏新帧通知 | 16 ms LVGL 定时轮询 | 任务通知立即唤醒 | Presenter 任务通知 |
| SPI DMA 数据 | 直接访问 PSRAM | 四行内部中转，DMA 不访问 PSRAM | PSRAM 直接 DMA |
| TE | 无 | 无，不等待 TE | 有 TE 同步逻辑 |

DOIT 采用有限的空闲/就绪队列，只有两个原生帧。送屏任务排空全部 SPI
事务之后才回收帧，错误路径也必须排空；旋转不会覆盖尚在传输的内存。
截图持有显示锁并等待已提交帧排空，从最后完成的原生帧反向旋转。
相比原方案，增加约 150 KiB PSRAM，并将 LVGL 两块内部缓冲从 24 行缩至
8 行，为 SPI 中转和其他外设保留 SRAM。

直通路径仅接受铺满屏幕、无偏移的 RGB565 1×/2×/4× 帧。缩放与旋转共享一次
复制，没有降低默认渲染分辨率。弹窗、透明层、鼠标、非全屏 Surface 和 ARGB
继续使用 LVGL 合成；直通获取遵守系统弹窗关闭后的新完整帧屏障。需要合成时
不提前消费 GuestMapped Surface，避免在 LVGL 读取前归还应用像素缓冲。

本次未复制 pai-touch 的 TE 或双核旋转线程。已有并行光栅线程使用核心 0，
完整绘制和送屏流水线比增加竞争同一核心的旋转线程更适合先验证。
80 MHz 传输 153600 B 的纯线速下限为 15.36 ms，实际还包含命令、排队与复制。
无 TE 时仍可能看到竖向撕裂。

## 实机记录

同一台设备、同一 Jump Jump 0.1.7 包，优化前实际显示约 17 FPS；首轮优化
在约 21.84 s 的已完成帧窗口内送出 1019 帧，约 **46.66 FPS**，截图浮层
为 47.8 FPS。两轮没有锁定玩家操作与镜头，属于现场对照，不是固定场景基准。
计数来自 LCD 完成后的帧号和时间戳，不是 Guest 的提交次数。

最终固件补齐 PXADB `PERF START display` 的 LCD 完成统计。在无截图干扰的
12.028344 s 窗口中，跳一跳完成 608 帧，即 **50.55 FPS**；Voxel 0.12.6
在 10.023221 s 的复杂世界窗口中完成 184 帧，即 **18.36 FPS**。
两者均为原生 320×240 渲染。PERF 的间隔数组容量为 256，超出时只丢弃间隔
样本，`completed/window_us` 仍覆盖整个窗口。

Voxel Craft 原生 320×240、视距 32、质量 q=0 的不同世界/视角中，日志约
16–28 FPS。复杂视角可出现 600 多个面、约 30 ms 原生光栅耗时，不能将
跳一跳的帧率当作所有体素场景的保证。继续提高这类场景的帧率需要针对
建模、遮挡和光栅成本测量。

首轮和最终固件各连续三次 Voxel/桌面/Jump Jump 切换，含 Ogg 音频、背包与
系统返回，没有观察到重启或音频欠载。最终固件使用系统默认的音频任务调度，
已移除诊断期间尝试过的音频固定核心选项。此前观察到的原生访问异常没有在
本轮复现，尚不能据此宣称已完成长期稳定性验证。

验证日志位于本地构建目录：

- `build/tests/doit-gamekit/games/device/display-pipeline-check.log`
- `build/tests/doit-gamekit/games/device/display-switch-check.log`
- `build/tests/doit-gamekit/games/device/display-final-check.log`：最终固件的实际送屏统计与三轮切换。
- `/tmp/doit-display-final-tests.log`：ASan/UBSan 输入、合成与双方向旋转、1×/2×/4× 缩放。
- `/tmp/doit-voxel-feel-test.log` 和 `/tmp/doit-voxel-feel-final-sanitized.log`：Voxel 手柄和游戏回归。
- `build/tests/doit-gamekit/games/device/voxel-final-package-check.log`：最终 Voxel 0.12.7 包及手柄提示截图。

板卡固件只更新应用分区；现有系统数据、已安装应用和存档保留。

最终固件应用分区镜像 SHA-256：
`7a1b0c62e5f9676c38959d4b970ceb0ace430fb0268f66bc1dcb81d3d4157ee1`。
