# GameKit / Korvo 启动白屏修复

两块板原先都会在系统界面尚未创建时运行 LVGL，首帧因此显示默认的白色背景。随后触摸、音频、Wi-Fi、存储及字体初始化继续执行，造成持续数秒的空白白屏。

现在使用共享的深色 `PXA / Starting...` 启动页，只依赖 LVGL 内置字体。GameKit 在显示初始化后立即绘制，等完整 SPI 帧进入 LCD 才打开背光，然后初始化其他硬件。Korvo 在创建显示期间持有 LVGL port 锁，避免后台任务抢先刷白；显示创建后立即提交启动帧，再初始化触摸、按键、音频、Wi-Fi 和文件系统。两者在系统界面创建完成后移除启动内容并主动刷新，恢复界面也可移除启动内容。

Korvo 的 RGB 缓冲原已由驱动清零，本次没有额外清空大帧缓冲或改变扫描参数。该板没有可控背光引脚，上电至 LCD 初始化之前的面板表现仍由硬件决定。

## GameKit 真机结果

设备为 `doit-gamekit` / ESP32-S31，序列号 `1c:29:04:d0:af:5e`。先对旧固件采集两次重启，再对最终固件采集三次重启。以下使用 ESP-IDF 控制台时间戳；测量为软件重启，未测断电重新上电，也没有用摄像机测量液晶像素响应。

| 阶段 | 旧固件 | 最终固件 |
| --- | --- | --- |
| 首次显示 | 1.618–1.619 秒，默认空白白屏 | 1.633–1.634 秒，启动内容整帧送屏后点亮背光 |
| SD 挂载 | 1.966–1.967 秒 | 2.026–2.027 秒 |
| SD 元信息及卷注册完成 | 未单独记录 | 2.028–2.029 秒 |
| 系统字体完整校验完成 | 4.084 秒 | 4.148–4.149 秒 |
| 系统界面就绪 | 旧版本无对应标记 | 一次完整捕获为 8.769 秒 |

最终三次均完成 PXADB HELLO 并取得正常桌面截图。UART 从 115200 切换为 2M 时会丢失部分采集标记，另外两次没有捕获到 UI 就绪行，不能据此推断启动失败或其完整耗时。日志标记统一使用 `esp_log_timestamp()`，与控制台时间基准一致；UI 就绪时间先保存，在调试串口切换速率后输出，避免标记被切换打断。

本次消除了等待期间的空白白屏，**没有证据表明完整加载时间明显缩短**。首帧同步送屏会增加约 60 ms 的 SD 启动前等待。字体大小和 SHA-256 完整校验仍花费约 2.12 秒，Host 初始化约 1.1–1.2 秒，之后还有字体与系统界面创建。曾试验 32 KiB stdio 缓冲，对启动没有明显收益，最终固件已撤回该实验，没有跳过字体校验。

只刷写 `0x10000` 应用固件区，刷写前核对板卡、芯片、MAC、现有分区表及应用区容量并备份原应用区和 NVS。写入通过 esptool 校验，随后读回确认 NVS 和分区表逐字节不变。五个已安装应用的身份、版本及启用状态一致；启动日志仍恢复已保存的 70% 音量。没有写入 SD 卡、应用数据或文件系统镜像。

指标与证据：[gamekit.json](boot-screen-20261019/gamekit.json)、[最终完整启动日志](boot-screen-20261019/gamekit-after-2.log)、[旧版日志](boot-screen-20261019/gamekit-before-1.log)、[桌面截图](boot-screen-20261019/gamekit-home.jpg)。原始日志及 NVS 备份仅留在本地，未提交。

## 构建与检查

两个板卡均使用 ESP-IDF 6.2 构建，通过应用区容量检查和 codec 内存审计。构建采用之前冻结的干净 SDK `dc9365e2da4a4187991a22b40e1276f26a2cc448`，排除其他任务未提交的 SDK 修改。实际固件、配置和源文件 SHA-256 见 [firmware.json](boot-screen-20261019/firmware.json)。可直接检查的本地产物位于 `local/releases/firmware/boot-screen-20261019/`，内含两板镜像、配置、审计记录和 `SHA256SUMS`。

| 板卡 | 固件大小 | 物理验证 |
| --- | --- | --- |
| doit-gamekit | 3,926,736 字节 | 已刷入最终镜像并完成上述三次重启检查 |
| esp32s31-korvo-1 | 3,940,096 字节 | 当时未连接，完成构建与宿主验证，尚未真机复验 |

共享启动页检查使用真实 LVGL、ASan/UBSan，实际收集 320×240 和 800×480 首帧的 RGB565 像素，验证深色背景、文字、无白色像素、重复显示/移除和系统界面交接。既有启动顺序测试继续检查两种挂载顺序、字体密度和失败路径，测试桩补齐时间戳函数。两板各自输入/显示宿主检查、音量立即重启恢复检查均通过。

```sh
bash firmware/components/pxa_board_common/tests/test_boot_screen.sh
python3 firmware/components/pxa_integration/tests/test_startup.py
python3 firmware/components/pxa_integration/tests/test_volume_preferences.py
bash firmware/boards/doit-gamekit/tests/test_host.sh
bash firmware/boards/esp32s31-korvo-1/tests/test_host.sh
```

启动页检查需要现有真实 LVGL 测试库；缺失时脚本会提示先运行 `tools/test-game-system-gestures.sh` 构建它。
