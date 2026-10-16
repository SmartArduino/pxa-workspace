# ESP-Mosaico ROM USB 握手排查

日期：2026-10-16。设备 ESP32-S31 rev0.0，ROM USB `303a:0020`，
序列号 `1c:29:04:d0:ad:04`；应用 USB 为 `303a:4001`。

## 已确认

- 报错现场确实枚举为 ROM 下载接口，不是应用 CDC；没有其他进程占用串口，
  ModemManager 和 brltty 均未运行。
- `Connecting ... No serial data received` 出现在 SYNC 握手阶段，尚未擦写 Flash，
  与 bootloader 剩余空间无关。
- 在当前这次枚举上，以先前成功的 `--before no-reset --no-stub` 参数重试仍无回复。
  因此仅禁用 stub 不能修复本次已经失效的握手。
- Linux 现场配置为 `power/control=auto`、`autosuspend_delay_ms=2000`，
  ROM 接口空闲后 `runtime_status=suspended`。打开 CDC 后会回到 active，仍无 SYNC 回复。
- 只读打开该设备的 usbfs 节点并保持句柄，可在不打开 CDC 的情况下保持 active。
  新工具在真机上持续等待 4 秒后仍为 active；退出后不修改原电源配置。

**推测而非定论：** rev0 ROM USB 在这条链路上经历休眠/恢复后可能失去通信能力。
“串口出现后等编译，随后握手超时”与此吻合；仍需在一次新的 ROM 枚举后，
立即保持 USB 活跃、等待超过两秒、再做 SYNC/完整写入，才能判断是否解决根因。
不能把运行时状态变化本身当作 Flash 写入成功的证据。

## 已实施的工作流修正

`tools/firmware.sh esp-mosaico` 的 flash、app-flash、bootloader-flash 和
partition-table-flash 动作会先运行 `tools/usb_keep_awake.py`，随后才等待共享构建锁
和运行 ESP-IDF。通过 `--port` 或 `ESPPORT` 确定物理 USB 位置；重新枚举时更新只读句柄。
它不打开 CDC，不发送 DTR/RTS，不修改系统 power/control，不需要管理员权限。
串口监视和其他板型维持原路径。

这符合 Linux 文档描述的 usbfs 行为：持有打开的 usbfs 文件时，设备不会被视为空闲。
参见 [Linux USB 电源管理文档](https://docs.kernel.org/driver-api/usb/power-management.html#when-is-a-usb-device-idle)。

如果 ROM 已在工具启动前失去响应，保持活跃不能回溯修复。应先启动烧录命令，
再让设备重新进入下载模式。旧固件不能通过此应用 USB 自动进入 ROM，仍需按键操作。
ROM 与应用重新枚举后 ttyACM 编号可能改变，需确认端口。

另外，原来默认启用的 stub 仅有外接 UART 成功记录；之前原生 USB 下载约 4.5%
时曾掉线。板型默认改为 `CONFIG_ESPTOOLPY_NO_STUB=y`，本地保存的 Mosaico 配置同步修正。
ROM 模式下 esptool 默认不压缩。外部 UART 可在验证后单独启用 stub。
此项解决的是握手后的另一处风险，不将其混作当前 SYNC 超时的根因。

推荐只更新应用分区，保留数据：

```sh
tools/firmware.sh esp-mosaico app-flash --port /dev/ttyACM2 --baud 2000000
```

使用 `idf.py` 原生命令时，可以显式套上同一个工具：

```sh
python tools/usb_keep_awake.py --port /dev/ttyACM2 -- \
  idf.py -C firmware -B build/firmware/esp-mosaico -DPXA_BOARD=esp-mosaico \
  -p /dev/ttyACM2 -b 2000000 app-flash
```

## 验证与未完成部分

七项自动检查已通过：只读打开与释放、重新枚举换节点、短暂节点缺失恢复、
不触碰其他 USB 设备、串口符号链接、子命令失败释放、构建锁前启用与参数保留。

```sh
python -m unittest discover -s tools -p test_usb_keep_awake.py
```

现场日志和对照脚本位于本地 `local/mosaico-usb-flash-20261016/`。
新的 ROM 枚举后的握手与写入对照尚待设备复位；目前没有新增 Flash 写入。
上一轮性能浮层候选固件仍待真机验证，不把本次主机工具自测当作该固件已经刷入。
