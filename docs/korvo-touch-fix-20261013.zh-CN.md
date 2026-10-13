# Korvo-1 触摸修复（2026-10-13）

设备为 ESP32-S31-Korvo-1，`/dev/ttyUSB0`，PXADB 使用 2,000,000 baud。
启动日志确认 GT1158（GT1151 驱动）已识别，五个独立 LVGL 指针设备已创建。
初始化失败不是本次已观察到的现象。

## 修正

**后续修正：用户实际操作反馈出现频繁误松手。下文所述“驱动返回零触点立即释放”
并不正确，已由[触摸稳定性修复](korvo-touch-stability-20261013.zh-CN.md)替代。**
GT1x 的未就绪邮箱不能当作有效零触点报告，也不应被确认清空。
下文保留为第一轮实现和验证记录，不能作为当前协议处理的说明。

原板级代码复制了 GT1151 的寄存器解析，但跳过了“触点数为零且 ready 位为零”的确认写入，
并将该报告解释为“没有更新”，最长 150 ms 后才释放。官方驱动会确认零触点报告。
这一协议差异已通过代码对照确认；它是否完整解释用户设备的失效，需要物理操作验证。

现在直接调用 `esp_lcd_touch_read_data/get_data` 读取、校验、确认并消费一次控制器报告，
多个 LVGL 指针共享这一份快照，不重复消费。
有效零触点报告立即释放；150 ms 超时只用于通信或校验失败，防止触点卡住。
固定容量的状态保持多指 ID，过滤重复 ID 和越界坐标，并在槽位改属新手指前先发送释放。
删除重复协议解析，没有新增任务、队列或动态分配；原生状态布局从 64 B 减为 40 B。

新增可选 `PXA_KORVO_TOUCH_TRACE`，默认关闭。打开后只记录物理指针的按下/松手边沿，
不把 PXADB 注入当作物理触摸。临时验证后恢复关闭。
早期 console 使用 115200，PXADB 启动后才改为 2 Mbaud。
第一轮尝试将缓存中的波特率设为 2 Mbaud，后续发现 ESP-IDF 的默认 UART 选择
会将其重新生成成 115200，因此单改该值不能改变早期 console 波特率。
后续验证通过分阶段切换主机串口波特率收集驱动启动日志，没有修改用户设置。

## 验证范围

- GT1151 初始化及五个物理 LVGL 输入设备：启动日志确认。
- 立即释放、短暂错误/超时恢复、五指/重排/重复 ID、坐标边界：ASan/UBSan 通过。
- PXADB 多指 mailbox、LVGL 输入锁 2999 次回调、游戏/系统手势/透明覆盖：通过。
- 开发工具 13 项：通过；ESP-IDF 固件构建与应用分区写入校验通过。
- 物理点击、拖动、双指、长按及松手：需要实际碰屏幕，不能由串口注入或模拟器替代。
  当前采集窗口没有收到手指事件；在用户确认之前，该项保持待验证。

仅写入 `0x10000` 应用分区，未擦除或烧写 NVS/应用数据分区，保留已安装应用和数据。
原始启动、构建、烧录及物理事件采集日志在 `local/korvo-touch-fix-20261013/`。
设备最终固件哈希与状态见同目录 `result.json`。

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
