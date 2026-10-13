# Korvo-1 误松手修复（2026-10-13）

本轮针对用户反馈的“手指未离开屏幕，却经常被识别为松手”。设备为
ESP32-S31-Korvo-1，`/dev/ttyUSB0`，2 Mbaud，800 × 480。
实际控制器启动标识为 GT1158，由 GT1151 1.1.1 驱动初始化。

## 原因与 pai-touch 的区别

上一轮直接用 `esp_lcd_touch_read_data/get_data` 返回的触点数判断松手，
遗漏了 GT1x 报告的 data-ready 位。驱动清空已消费的缓冲后，尚无新报告时同样返回
零触点；在 6 ms 轮询中将它解释为松手，会产生虚假的 UP/DOWN。
这是一处软件协议处理错误，不能靠拉长松手延时解决。

GT1x 的 `0x814e` 是需要确认的报告邮箱：bit 7 为就绪位，低四位为触点数。
`0x00` 表示没有就绪报告，`0x80` 才是就绪的零触点报告。
[Goodix 官方 GT1x 驱动](https://github.com/goodix/gt1x_driver_generic/blob/master/gt1x.c)
在 ready 位为零时跳过处理和确认写入；
[Linux Goodix 驱动](https://github.com/torvalds/linux/blob/master/drivers/input/touchscreen/goodix.c)
也区分未就绪和零触点。此前关于“未就绪零触点也必须确认”的判断不成立。

pai-touch 的本地 CST826 驱动每次从 `0x02` 重新取得当前触点记录，
板级代码还使用实际连接的触摸 IRQ 唤醒 LVGL。Korvo 官方板描述将 touch interrupt
设为 `-1`，当前不能通过软件获得同样的硬件中断。因此借鉴其共享快照、固定多指 ID、
输入时间戳及短暂通信故障保护，保留适合 GT1x 的报告就绪判断。

## 实现与代价

| 改动 | 收益 | 内存/行为 |
| --- | --- | --- |
| 读取 ready 位后才调用官方驱动消费报告 | 空闲轮询不再伪造松手；不清空尚未就绪的邮箱，避免清除恰好到达的新报告 | 无额外持久状态；继续复用官方坐标解析、校验和确认 |
| 无新报告时保留触点，并更新通信健康时间 | 静止长按不限于旧的 150 ms；一次短暂通信故障不因手指静止过久立即释放 | 既有 64 位时间字段改语义，状态仍为 40 B |
| 就绪零触点立即释放 | 真正松手不添加去抖等待 | 原有 6 ms 轮询周期；150 ms 仅限制持续通信故障 |
| 400 kHz I²C、20 ms 单次传输超时 | 缩短读取与故障阻塞；400 kHz 与 Espressif 官方 Korvo 板配置一致 | 没有新任务、队列、堆分配 |
| 轮询窗口从传输结束计算 | 慢传输后下一个指针不立即再次消费另一份报告 | 复用既有轮询时间字段 |
| 越界位置保留已知 ID 的最后有效坐标 | 边缘异常坐标不产生误松手；新出现的无效触点仍被过滤 | 固定容量数组；重复 ID、重排及槽位更换规则保留 |

诊断选项 `PXA_KORVO_TOUCH_TRACE=ON` 每秒记录报告/空闲/错误计数、平均与最大
读取耗时，并记录物理指针边沿。默认关闭，不记录 PXADB 注入。

## 验证

- 原生 ASan/UBSan：报告生成慢于轮询、持续 60 秒静止长按、真实零触点立即释放、
  256 种状态字节、无效报告/读取失败、150 ms 故障期限、五指重排/替换、重复 ID、
  越界坐标恢复通过。
- 负向验证：分别去掉 ready 判断、把未就绪处理改回释放，报告回放测试均失败。
  因此测试能够抓住本次问题，而非仅检查新实现的正常调用。
- PXADB 输入 mailbox、LVGL 2999 次串行化回调、游戏与全屏系统手势回归通过。
- ESP-IDF 诊断与生产固件构建通过；应用分区写入及校验结果、原始诊断日志见
  `local/korvo-touch-stability-20261013/`。
- 真机 150 个一秒采样区间：20,091 次空闲轮询，平均读取 244.165 µs，
  最大 564 µs，无通信错误、就绪报告或物理指针边沿。
  这是报告读取耗时，不是触摸到显示的端到端延迟，也不是物理手指稳定性的证明。
- 解锁后实际运行 Maze Evil，串口注入 DOWN → MOVE → UP，确认应用仍活动，
  从实际面板帧缓冲取得 800 × 480 游戏截图（`maze-final.jpg`），随后停止游戏。
  这是应用路由/显示检查，不能用来证明物理触摸已消除断触。
- 物理长按、连续拖动、双指移动与松手需要实际手指参与；结果在本轮日志/结果文件中
  单独记录，不能用串口注入或模拟器代替。

固件仅烧写 `0x10000` 应用分区，不擦除已安装应用、NVS 或数据分区。
没有修改用户 Wi-Fi、音量等设置。临时诊断选项在最终固件中关闭。
测试后重启清除临时串口注入输入设备。早期 console 为 115200 baud，PXADB 为
2 Mbaud；启动采集按阶段切换主机串口，最终主机通信恢复 2 Mbaud。
最终二进制为 3,739,152 B，比上一轮二进制增加 80 B；这不是运行时堆或队列增长。

```sh
bash firmware/boards/esp32s31-korvo-1/tests/test_host.sh
bash tools/test-pxadb-input.sh
bash tools/test-lvgl-input-lock.sh
bash tools/test-game-system-gestures.sh

source /home/lucinhu/esp/esp-idf/esp-idf-main/export.sh
idf.py -C firmware -B build/firmware/esp32s31-korvo-1 \
  -DPXA_BOARD=esp32s31-korvo-1 -DPXA_KORVO_TOUCH_TRACE=OFF build
tools/firmware.sh esp32s31-korvo-1 app-flash --port /dev/ttyUSB0 --baud 2000000
```

剩余限制：本轮没有改变 Guest 帧时间、Host 光栅化或显示排队，低 FPS 游戏的显示响应
依旧受这些阶段影响；触摸报告正确性与游戏显示帧率是两项不同的测量。
