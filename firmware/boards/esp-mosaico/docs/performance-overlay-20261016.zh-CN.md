# 性能浮层统计修正

日期：2026-10-16。接续 [UI 快照流水线优化](ui-parallel-pipeline-20261016.zh-CN.md)。

## 复现与原因

旧固件在设置页连续滚动 6,586,951 µs，PXADB `PERF START display` / `PERF STOP`
记录 172 个完成帧，即 **26.112 FPS**，结束截图的顶部却为 `FPS01 9`。
截图逐像素读数确认显示的是 1.9，小数点像素缺失。数据与截图保存在本地
`local/mosaico-hud-20261016/`。

- Mosaico 原先在绘制提交时计数，尚未知道 LCD 传输是否完成。
- 浮层只画进 LVGL 当前脏矩形。设置列表在顶部下方滚动时，FPS 数值没有进入传屏区域。
- 小字体缺少 `.` 字形。
- pai-touch 的 `R` 只累计普通 UI 旋转，遗漏游戏直出；编译期关闭性能日志时，
  统计字段和相关计时也被关闭，浮层路径还会引用不存在的字段。

## 修改

Mosaico 在成功传输完整内容刷新后计数，与独立的 PXADB display 统计同处一个完成分支。
传屏任务在已有不可变快照上绘制 FPS；读数改变时将顶部矩形纳入本次传输。
没有内容提交时，复用同一任务和现有缓冲更新顶部 60×16 矩形，按面板要求取整。
这些诊断传输不递增内容帧计数，也不计入 PXADB PERF。
关闭浮层请求完整 UI 重绘，以恢复背景；游戏退出后恢复 UI 与空闲诊断更新共享面板锁。
诊断传输失败请求完整重绘，并与下一次内容刷新的错误状态隔离。

共享采样约每秒更新一次，支持空闲采样归零和 32 位微秒计数回绕；完成计数与配置采用
原子访问，采样状态以一个字节保存。补上小数点字形。

pai-touch 仍显示 `FPSxx.x Rn Mn`，采样周期由两秒改为一秒：

- `FPS`：窗口内完成 LCD 传输的帧率。
- `R`：普通 UI 和游戏直出帧，按帧数加权的平均旋转、转换及相关合成耗时。
- `M`：平均传屏耗时，包含等待 LCD 传输完成，不含此前队列等待。

`R`、`M` 以整数毫秒显示，不含 Guest 更新和 Host 光栅化，不能相加换算 FPS。
统计不再依赖编译期性能日志开关；窗口采样与重置由旋转锁串行化。
pai-touch 没有新内容帧时，屏幕保持最后一个样本，不为诊断单独刷新全屏。

## 开销与已验证项目

没有增加任务、队列深度、帧缓冲、DMA 缓冲或动态分配。
Mosaico 与上一轮实际刷入的异步快照固件对比：

| 链接段 | 修改前 / 后字节数 |
| --- | --- |
| SRAM data | 39,196 / 39,196 |
| SRAM bss | 31,320 / 31,320 |
| IRAM text | 105,806 / 105,806 |
| PSRAM bss | 36,016 / 36,016 |
| Flash text | 3,045,186 / 3,046,218 |

pai-touch 默认配置的 `Context` 前后均为 **952 B**；关闭编译期日志后为 944 B。
后者现在可以编译，不将此前无法编译的配置当作有效运行基线。
空闲诊断有少量额外传输；关闭浮层后不增加传屏任务的周期唤醒。

已通过：

- 共享统计 ASan/UBSan：正常帧率、首帧边界、空闲、计时回绕、小数点、分块绘制、
  禁用浮层以及并发完成计数。
- Mosaico 原生合成、刷新区间、DMA、输入和音频回归。
- 真实 LVGL 9.6 模拟器：滚动、透明与圆角逐像素对照，延迟快照所有权、
  游戏到 UI 恢复及双指输入。
- 两块板子的完整固件构建；pai-touch 生产显示代码在性能日志 1/0 两种配置下编译。

复现命令从仓库根目录执行；固件构建使用已导出的 ESP-IDF 环境：

```sh
bash firmware/components/pxa_board_api/tests/test_host.sh
bash firmware/boards/esp-mosaico/tests/test_host.sh
flock .pxa/firmware-build.lock bash firmware/boards/esp-mosaico/tests/test_lvgl.sh --gestures
tools/firmware.sh pai-touch build
flock .pxa/firmware-build.lock python firmware/boards/pai-touch/tests/test_performance_build.py
tools/firmware.sh esp-mosaico build
```

待真机刷入后补充读数与独立完成帧率的对照、稳定与窗口最低可用堆、画面回归。
固件 SHA256：`81295fbe865912b820d47dab91468ff87c7074d3906b22b1d6ff361f5b05e433`。
本轮修正统计工具，不将浮层读数变化作为游戏或 UI 性能提升的证据。
