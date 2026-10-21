# 系统默认时间、保存恢复与 RTC 接口验证

未校准时，状态栏、锁屏和通知面板使用持续走动的系统日历时间。首次无有效来源时
从本地时间 `2026-01-01 00:00:00` 开始。启动来源依次为板级 RTC 回调、有效的
系统保留时间、NVS `pxa_clock/utc` 和默认时间。接口和板卡接入见
[pxa_system_clock](../../firmware/components/pxa_system_clock/README.md)。

## 自动测试和构建

- 生产时间服务的 ASan/UBSan 主机测试通过：默认时间走动、UTC 保存恢复、
  软件重启回调、时区切换、RTC 优先级、只读来源、错误回退和写入失败。
- 生产时间服务与真实 LVGL 集成测试通过：初始状态、状态栏/通知面板/锁屏刷新、
  跨日和重启恢复。
- 启动顺序测试通过：时间在 Host 和系统 UI 之前就绪，不依赖 PXA 文件系统挂载。
- Pai Touch、GameKit-1 和 GameKit 完整构建通过；GameKit 的 codec memory audit 通过。

## 实机结果

GameKit-1 已写入应用镜像并通过写入哈希校验，运行 ELF 为
`f65acf6ae677f520cbc20153252ce138dface4aabbc10c0c766fdf099bb3eb50`。
[启动截图](system-clock-20261021/boot.png) 显示默认 `00:00`。该设备随后被另一个
GB 性能测试占用，完整计时验证在空闲的 GameKit 上完成。

GameKit 芯片序列号为 `1c:29:04:d0:af:5e`，运行 ELF 为
`fe5aa99a92288e5c19ab07a3eaad10582b3319e33c093abc068db11106bdf7da`，
已与固定镜像核对。测试期间未修改时区或手动设置时间。

| 检查 | 观察结果 | 证据 |
| --- | --- | --- |
| 启动默认时间 | 状态栏 `00:00` | [截图](system-clock-20261021/doit-gamekit/live-before-restart.png) |
| 跨分钟走时 | 约 78 秒时状态栏 `00:01` | [截图](system-clock-20261021/doit-gamekit/after-minute.png) |
| 自动锁屏 | 约 138 秒时显示 `00:02 / 2026-01-01` | [截图](system-clock-20261021/doit-gamekit/locked.png) |
| 软件重启恢复 | 启动后仍显示 `00:02` | [截图](system-clock-20261021/doit-gamekit/restored.png) |
| 恢复后继续走时 | 再等待 65 秒，状态栏显示 `00:03` | [截图](system-clock-20261021/doit-gamekit/restored-ticking.png) |
| 通知面板 | 显示 `00:03 / 2026-01-01` | [截图](system-clock-20261021/doit-gamekit/notification-date.png) |
| 硬复位恢复 Flash 检查点 | 回到此前保存的 `00:02` | [截图](system-clock-20261021/doit-gamekit/after-hard-reset.png) |

通过 ROM 只读回 NVS，并使用 ESP-IDF 自带解析器仅提取 `pxa_clock/utc`，
确认软件重启实际提交了 **1767196943**，即 UTC `2025-12-31 16:02:23`、
本地 `2026-01-01 00:02:23`。对应条目及页头 CRC 均正确。
原始分区只存在于私有临时目录，读取后已删除；报告仅保留
[时间检查点](system-clock-20261021/doit-gamekit/checkpoint.json)。
退出 ROM 后硬复位，设备从该检查点恢复 `00:02`，验证了 Flash 恢复路径。
硬复位不执行应用关机回调，因此恢复的是上次软件重启保存的时间。

## 复现与范围

完整截图元数据、运行哈希、镜像 SHA-256 和人工图像复核结论保存在
[device-test.json](system-clock-20261021/doit-gamekit/device-test.json)。
复现脚本为 [run_device_test.py](system-clock-20261021/run_device_test.py) 和
[read_checkpoint.py](system-clock-20261021/read_checkpoint.py)，均要求显式指定串口。

GameKit 本轮采用成功完整构建后的固定应用镜像，位于
`build/validation/system-clock-20261021/doit-gamekit/pxa_esp_platform.bin`，
镜像 SHA-256 为 `aaf58e27e4e15a3db656e4ecde58577916234cddbb9fcedb007d400f3f9e59d3`。
常规 `app-flash` 的隐式重编译曾遇到并行工作区改动造成的
`pxa_private_data_move.h` 缺失，随后直接烧录上述固定镜像并完成写入校验。

这两款 GameKit 的电源由硬件开关控制，未进行物理断电测试。可控关机板卡的保存
调用点已接入；外置 RTC/协处理器的回调、断电期间走时模型及失败回退已通过主机
测试，尚未连接独立 RTC 硬件实测。无后备时钟时，开机从最后保存的时刻继续。
